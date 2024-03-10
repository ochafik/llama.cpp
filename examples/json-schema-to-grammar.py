#!/usr/bin/env python3
import argparse
from dataclasses import dataclass
import itertools
import json
import re
import sys
from typing import Any, Dict, List, Optional, Set, Tuple, Union

# whitespace is constrained to a single space char to prevent model "running away" in
# whitespace. Also maybe improves generation quality?
SPACE_RULE = '" "*'

PRIMITIVE_RULES = {
    'boolean': '("true" | "false") space',
    'number': '("-"? ([0-9] | [1-9] [0-9]*)) ("." [0-9]+)? ([eE] [-+]? [0-9]+)? space',
    'integer': '("-"? ([0-9] | [1-9] [0-9]*)) space',
    'value'  : 'object | array | string | number | boolean',
    'object' : '"{" space ( string ":" space value ("," space string ":" space value)* )? "}" space',
    'array'  : '"[" space ( value ("," space value)* )? "]" space',
    'string': r''' "\"" (
        [^"\\] |
        "\\" (["\\/bfnrt] | "u" [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F])
      )* "\"" space ''',
    'null': '"null" space',
}

INVALID_RULE_CHARS_RE = re.compile(r'[^a-zA-Z0-9-]+')
GRAMMAR_LITERAL_ESCAPE_RE = re.compile(r'[\r\n]')
GRAMMAR_LITERAL_ESCAPES = {'\r': '\\r', '\n': '\\n'}


def resolve_refs(schema: dict):
    def visit(n: dict):
        if isinstance(n, list):
            return [visit(x) for x in n]
        elif isinstance(n, dict):
            ref = n.get('$ref')
            if ref is not None:
                if ref.startswith('#/'):
                    target = schema
                    name = None
                    for sel in ref.split('/')[1:]:
                        name = sel
                        assert target is not None and sel in target, f'Error resolving ref {ref}: {sel} not in {target}'
                        target = target[sel]
                    
                    return target
                else:
                    raise ValueError(f'Unsupported ref {ref}')
            else:
                for k in n.keys():
                    v = n[k]
                    vv = visit(v)
                    if vv is not v:
                        n[k] = vv
        else:
            pass
        return n
    return visit(schema)


@dataclass
class Rule:
    def optimize(self):
        raise NotImplementedError(f'{self.__class__.__name__}.optimize() not implemented')

    def __str__(self):
        raise NotImplementedError(f'{self.__class__.__name__}.__str__() not implemented')

@dataclass
class LiteralRule(Rule):
    literal: str

    def optimize(self):
        return self

    def __str__(self):
        return json.dumps(self.literal)

def _format_range_char(c):
    if c in ('-', ']', '\\'):
        return '\\' + c
    elif c == '\n':
        return '\\n'
    elif c == '\r':
        return '\\r'
    elif c == '\t':
        return '\\t'
    else:
        return c
    
@dataclass
class RangeRule(Rule):
    negated: bool
    items: List[Union[Tuple[str, str], str]]

    def optimize(self):
        return self

    def __str__(self):
        def format_item(item):
            if isinstance(item, tuple):
                return f'{_format_range_char(item[0])}-{_format_range_char(item[1])}'
            else:
                return _format_range_char(item)
        return f'[{("^" if self.negated else "")}{"".join(map(format_item, self.items))}]'

@dataclass
class UnionRule(Rule):
    alt_rules: List[Rule]

    def optimize(self):
        positive_range_items: list[Union[str, Tuple[str, str]]] = []
        negative_range_items: list[Union[str, Tuple[str, str]]] = []
        others: list[Rule] = []
        for r in self.alt_rules:
            if isinstance(r, LiteralRule) and len(r.literal) == 1:
                positive_range_items.append(r.literal)
            elif isinstance(r, RangeRule):
                (negative_range_items if r.negated else positive_range_items).extend(r.items)
            else:
                others.append(r)

        rule = UnionRule(alt_rules=[])
        if positive_range_items:
            rule.alt_rules.append(RangeRule(negated=False, items=positive_range_items))
        if negative_range_items:
            rule.alt_rules.append(RangeRule(negated=True, items=negative_range_items))
        rule.alt_rules.extend(others)
        return rule
        
    def __str__(self):
        def sub(r):
            s = str(r)
            return f'({s})' if isinstance(r, UnionRule) else s

        return ' | '.join(map(sub, self.alt_rules))

@dataclass
class SequenceRule(Rule):
    rules: List[Rule]

    def optimize(self):
        rules = []
        for t, g in itertools.groupby(self.rules, lambda x: x.__class__):
            if t == LiteralRule:
                rules.append(LiteralRule(literal=''.join(x.literal for x in g)))
            else:
                for x in g:
                    rules.append(x.optimize())
        return self.rules[0] if len(self.rules) == 1 else SequenceRule(rules=rules)

    def __str__(self):
        return ' '.join((str(r) for r in self.rules))

@dataclass
class RepeatRule(Rule):
    sub: Rule
    min_times: int
    max_times: Optional[int] = None

    def optimize(self):
        return RepeatRule(sub=self.sub.optimize(), min_times=self.min_times, max_times=self.max_times)
    
    def __str__(self):
        subs = str(self.sub)
        if isinstance(self.sub, (SequenceRule, UnionRule)):
            subs = f'({subs})'
        if self.min_times == 0 and self.max_times is None:
            return f'{subs}*'
        elif self.min_times == 0 and self.max_times == 1:
            return f'{subs}?'
        elif self.min_times == 1 and self.max_times is None:
            return f'{subs}+'
        else:
            return ' '.join([str(subs)] * self.min_times + 
                            ([f'{subs}?'] * (self.max_times - self.min_times) if self.max_times is not None else [f'{subs}*']))

@dataclass
class NamedRule(Rule):
    name: str

    def optimize(self):
        return self

    def __str__(self):
        return self.name

@dataclass
class ParenthezisedRule(Rule):
    sub: Rule

    def optimize(self):
        if isinstance(self.sub, ParenthezisedRule):
            return self.sub.optimize()
        return ParenthezisedRule(sub=self.sub.optimize())

    def __str__(self):
        return f'({self.sub})'

@dataclass(kw_only=True)
class Schema:

    @staticmethod
    def from_json(s: str):
        schema = json.loads(s)
        resolved_schema = resolve_refs(schema)
        return Schema._from_json(resolved_schema)

    def to_grammar(self, prop_order: Dict[str, int] = {}):
        builder = GrammarBuilder(prop_order)
        builder.visit(self, '')
        return builder.format_grammar()

    @staticmethod
    def _sub_name(name, prop_name):
        return f'{name}{"-" if name else ""}{prop_name}'
    
    @staticmethod
    def _generate_union(alt_schemas):
        return UnionSchema(alt_schemas=list(map(Schema._from_json, alt_schemas)))

    @staticmethod
    def _from_json(schema: dict) -> 'Schema':
        schema_type = schema.get('type')
        # ref = schema.get('$ref')

        if 'oneOf' in schema or 'anyOf' in schema:
            return Schema._generate_union(schema.get('oneOf') or schema['anyOf'])
        

        elif 'const' in schema:
            return ConstSchema(value=schema['const'])

        elif isinstance(schema_type, list):
            return Schema._generate_union([Schema._from_json({'type': t}) for t in enumerate(schema_type)])
        
        elif 'enum' in schema:
            return Schema._generate_union(None, [ConstSchema(v) for v in schema['enum']])

        elif schema_type in (None, 'object') and 'properties' in schema:
            required = set(schema.get('required', []))
            properties = schema['properties']
            additional_properties = schema.get('additionalProperties')
            return ObjectSchema(
                props={
                    k: Schema._from_json(v)
                    for k, v in properties.items()
                },
                required=required,
                additional_properties=additional_properties)

        elif schema_type == 'object' and 'allOf' in schema:
            required = set()
            properties = []
            additional_properties = []
            def add_component(comp_schema, is_required):
                # ref = comp_schema.get('$ref')
                # if ref is not None and (resolved := self._resolve_ref(ref)) is not None:
                #     comp_schema = resolved[1]

                if 'properties' in comp_schema:
                    for prop_name, prop_schema in comp_schema['properties'].items():
                        properties.append((prop_name, prop_schema))
                        if is_required:
                            required.add(prop_name)
                if 'additionalProperties' in schema:
                    additional_properties.append(Schema._from_json(schema['additionalProperties']))

            for t in schema['allOf']:
                if 'anyOf' in t:
                    for tt in t['anyOf']:
                        add_component(tt, is_required=False)
                else:
                    add_component(t, is_required=True)

            additional_properties = additional_properties[0] if len(additional_properties) == 1 \
                else UnionSchema(alt_schemas=additional_properties) if additional_properties \
                else None

            return ObjectSchema(
                props={
                    k: Schema._from_json(v)
                    for k, v in properties
                },
                required=required,
                additional_properties=additional_properties)

        elif schema_type == 'array' and 'items' in schema:
            # TODO `prefixItems` keyword
            items = schema['items']
            if isinstance(items, list):
                return TupleSchema(list(Schema._from_json, items))
            else:
                return ArraySchema(
                    item_schema=Schema._from_json(items),
                    min_items=schema.get("minItems", 0),
                    max_items=schema.get("maxItems"))
            
        elif schema_type in (None, 'string') and 'pattern' in schema:
            return PatternSchema(pattern=schema['pattern'])

        # elif ref is not None and ref.startswith('https://'):
        #     import requests
        #     ref_schema = requests.get(ref).json()
        #     return Schema._from_json(ref_schema, ref)

        elif (schema_type == 'object' and len(schema) == 1 or schema_type is None and len(schema) == 0) \
            or schema_type in PRIMITIVE_RULES:
            return PrimitiveSchema(type=schema_type)
        
        else:
            raise ValueError(f'Unrecognized schema: {json.dumps(schema, indent=2)}')

def join_lists(sep, lists):
    out = []
    for i, l in enumerate(lists):
        if i > 0:
            out.extend(sep)
        out.extend(l)
    return out

@dataclass(kw_only=True)
class ObjectSchema(Schema):
    props: Dict[str, Schema]
    required: Set[str]
    additional_properties: Optional[Schema]

@dataclass(kw_only=True)
class ArraySchema(Schema):
    item_schema: Schema
    min_items: int = 0
    max_items: Optional[int] = None

@dataclass(kw_only=True)
class TupleSchema(Schema):
    item_schemas: List[Schema]

@dataclass(kw_only=True)
class UnionSchema(Schema):
    alt_schemas: List[Schema]

@dataclass(kw_only=True)
class ConstSchema(Schema):
    value: str

@dataclass(kw_only=True)
class PrimitiveSchema(Schema):
    type: str

@dataclass(kw_only=True)
class PatternSchema(Schema):
    pattern: str

    def to_rule(self):
        assert self.pattern.startswith('^') and \
            self.pattern.endswith('$'), \
            'Pattern must start with "^" and end with "$"'
        pattern = self.pattern[1:-1]
        try:
            def visit(pattern):
                if pattern[0] == re._parser.LITERAL:
                    return LiteralRule(literal=chr(pattern[1]))
                
                elif pattern[0] == re._parser.NOT_LITERAL:
                    return RangeRule(negated=True, items=[chr(pattern[1])])
                
                elif pattern[0] == re._parser.ANY:
                    raise ValueError('Unsupported pattern: "."')
                
                elif pattern[0] == re._parser.IN:
                    def get_item(c):
                        if c[0] == re._parser.LITERAL:
                            return chr(c[1])
                        elif c[0] == re._parser.RANGE:
                            return (chr(c[1][0]), chr(c[1][1]))
                        else:
                            raise ValueError(f'Unrecognized pattern: {c}')
                    return RangeRule(negated=False, items=[get_item(c) for c in pattern[1]])
                
                elif pattern[0] == re._parser.BRANCH:
                    return UnionRule(alt_rules=[visit(p) for p in pattern[1][1]])
                
                elif pattern[0] == re._parser.SUBPATTERN:
                    return ParenthezisedRule(sub=visit(pattern[1][3]))
                
                elif pattern[0] == re._parser.MAX_REPEAT:
                    return RepeatRule(
                        sub=visit(pattern[1][2]),
                        min_times=pattern[1][0],
                        max_times=pattern[1][1] if not pattern[1][1] == re._parser.MAXREPEAT else None)
                
                elif isinstance(pattern, re._parser.SubPattern):
                    return SequenceRule(rules=[visit(x) for x in pattern.data])
                
                elif isinstance(pattern, list):
                    return SequenceRule(rules=[visit(x) for x in pattern])
                
                else:
                    raise ValueError(f'Unrecognized pattern: {pattern} ({type(pattern)})')

            return visit(re._parser.parse(pattern))
        except BaseException as e:
            raise Exception(f'Error processing pattern: {pattern}: {e}') from e


def _sub_name(name, prop_name):
    return f'{name}{"-" if name else ""}{prop_name}'

class GrammarBuilder:
    def __init__(self, prop_order):
        self._prop_order = prop_order
        self._rules = {'space': SPACE_RULE}
        self.ref_base = None

    def _add_rule(self, name, rule):
        assert isinstance(rule, Rule)
        rule = rule.optimize()
        esc_name = INVALID_RULE_CHARS_RE.sub('-', name)
        if esc_name not in self._rules or self._rules[esc_name] == rule:
            key = esc_name
        else:
            i = 0
            while f'{esc_name}{i}' in self._rules:
                i += 1
            key = f'{esc_name}{i}'
        self._rules[key] = rule
        return NamedRule(name=key)

    def visit(self, schema: Schema, name: str) -> Rule:
        assert isinstance(schema, Schema)
        rule_name = name or 'root'
            
        def lit_space(lit: str) -> list[Rule]:
            return [LiteralRule(literal=lit), NamedRule('space')]
            
        if isinstance(schema, UnionSchema):
            return self._add_rule(rule_name, UnionRule(alt_rules=[self.visit(s) for s in schema.alt_schemas]))
        elif isinstance(schema, ConstSchema):
            return self._add_rule(rule_name, LiteralRule(literal=schema.value))
        elif isinstance(schema, ObjectSchema):
            sorted_props = [
                kv[0]
                for _, kv in sorted(
                    enumerate(schema.props.items()),
                    key=lambda ikv: (self._prop_order.get(ikv[1][0], len(self._prop_order)), ikv[0])
                )
            ]
            
            prop_kv_rules = {}
            for prop_name, prop_schema in schema.props.items():
                prop_rule = self.visit(prop_schema, _sub_name(name, prop_name))
                prop_kv_rules[prop_name] = self._add_rule(
                    _sub_name(name, f'{prop_name}-kv'),
                    SequenceRule(rules=lit_space(f'"{prop_name}"') + lit_space(':') + [prop_rule])
                )

            required_props = [k for k in sorted_props if k in schema.required]
            optional_props = [k for k in sorted_props if k not in schema.required]
            
            rule = SequenceRule(
                rules=lit_space('{') + 
                    join_lists(lit_space(','), [
                        [prop_kv_rules[k]]
                        for k in required_props
                    ]))

            if optional_props:
                def get_recursive_refs(ks, first_is_optional):
                    [k, *rest] = ks
                    kv_rule = prop_kv_rules[k]
                    if first_is_optional:
                        res = RepeatRule(
                            sub=SequenceRule(rules=lit_space(',') + [kv_rule]),
                            min_times=0,
                            max_times=1
                        )
                    else:
                        res = kv_rule
                    if len(rest) > 0:
                        res = SequenceRule(rules=[
                            res,
                            self._add_rule(
                                _sub_name(name, f'{k}-rest'),
                                get_recursive_refs(rest, first_is_optional=True)
                            )
                        ])
                    return res

                combinations = UnionRule(alt_rules=[
                    get_recursive_refs(optional_props[i:], first_is_optional=False)
                    for i in range(len(optional_props))
                ])

                if required_props:
                    sub = SequenceRule(rules=lit_space(',') + [combinations])
                else:
                    sub = combinations
                rule.rules.append(RepeatRule(sub=sub, min_times=0, max_times=1))

            rule.rules.extend(lit_space('}'))
            return self._add_rule(rule_name, rule)
            # return self._add_rule(rule_name, self._build_object_rule(properties.items(), required, name))

            # elif schema_type == 'object' and 'additionalProperties' in schema:
            # additional_properties = schema['additionalProperties']
            # if not isinstance(additional_properties, dict):
            #     additional_properties = {}

            # sub_name = f'{name}{"-" if name else ""}additionalProperties'
            # value_rule = self.visit(additional_properties, f'{sub_name}-value')
            # kv_rule = self._add_rule(f'{sub_name}-kv', f'string ":" space {value_rule}')
            # return self._add_rule(
            #     rule_name,
            #     f'( {kv_rule} ( "," space {kv_rule} )* )*')
            # pass

        elif isinstance(schema, TupleSchema):
            return self._add_rule(rule_name, SequenceRule(rules=lit_space('[') + 
                                join_lists(lit_space(','), [
                                    lit_space(',') + [self.visit(s, _sub_name(name, i))]
                                    for i, s in enumerate(schema.item_schemas)
                                ]) + lit_space(']')))

        elif isinstance(schema, ArraySchema):
            return self._add_rule(rule_name, RepeatRule(
                sub=self.visit(schema.item_schema, _sub_name(name, 'item')),
                min_times=schema.min_items,
                max_times=schema.max_items))
            
        elif isinstance(schema, PatternSchema):
            return self._add_rule(rule_name, schema.to_rule())
        
        elif isinstance(schema, PrimitiveSchema):
            if schema.type == 'object':
                self._rules.extend(PRIMITIVE_RULES)
            else:
                self._rules[schema.type] = PRIMITIVE_RULES[schema.type]
            return self._add_rule(rule_name, NamedRule(schema.type))
        
        else:
            raise ValueError(f'Unrecognized schema: {json.dumps(schema, indent=2)}')

    def format_grammar(self):
        return '\n'.join((f'{name} ::= {rule}' for name, rule in self._rules.items()))


def main(args_in = None):
    parser = argparse.ArgumentParser(
        description='''
            Generates a grammar (suitable for use in ./main) that produces JSON conforming to a
            given JSON schema. Only a subset of JSON schema features are supported; more may be
            added in the future.
        ''',
    )
    parser.add_argument(
        '--prop-order',
        default=[],
        type=lambda s: s.split(','),
        help='''
            comma-separated property names defining the order of precedence for object properties;
            properties not specified here are given lower precedence than those that are, and are
            sorted alphabetically
        '''
    )
    parser.add_argument('schema', help='file containing JSON schema ("-" for stdin)')
    args = parser.parse_args(args_in)

    if args.schema.startswith('https://'):
        import requests
        schema = requests.get(args.schema).json()
    elif args.schema == '-':
        schema = json.load(sys.stdin)
    else:
        with open(args.schema) as f:
            schema = json.load(f)

    print(Schema.from_json(json.dumps(schema)).to_grammar())


if __name__ == '__main__':
    main()
