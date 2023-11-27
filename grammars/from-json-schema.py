# Generates a GBNF grammar from a JSON schema (https://www.schemastore.org/json/)
#
#   python grammars/from-json-schema.py https://json.schemastore.org/compile-commands.json
# 
# Pro-tip: Use projects such as typescript-json-schema to generate a JSON schema from a TypeScript type:
#
#   echo "export type Foo = { instruction: string, input?: string, output: string }" > foo.d.ts 
#   python grammars/from-json-schema.py <( npx typescript-json-schema --defaultProps --required foo.d.ts Foo )
import json, sys, requests, jsonargparse
from typing import Optional

schemas = {}

def get_schema(url):
  if url not in schemas:
    if url.startswith("https://"): 
      text = requests.get(url).text
    else:
      with open(url, 'rt') as f:
        text = f.read()
    schema = json.loads(text)
    schemas[url] = schema
    for k, v in schema.get('definitions', {}).items():
      schemas[f'#/definitions/{k}'] = v
  return schemas[url]

def join_iter(it, sep):
  for i, e in enumerate(it):
    if i > 0: yield sep
    if isinstance(e, list): yield from e
    else: yield e

def process_type(path, schema, defs, type_to_def_name={}):
  ref = schema.get('$ref', None)
  enum = schema.get('enum', None)
  properties = schema.get('properties', None)
  additional_properties = schema.get('additionalProperties', True)
  
  def get_ref(name_stub, definition):
    key = json.dumps(definition)
    existing = type_to_def_name.get(key, None)
    if existing is not None: return existing

    i = 1
    name = name_stub
    while name in defs:
      i = i + 1
      name = f'{name_stub}{i}'

    defs[name] = list(definition)
    type_to_def_name[key] = name
    return name

  if ref is not None:
    yield from process_type(path + [ref], get_schema(ref))
  elif enum is not None:
    yield from ['(', *join_iter([f'"{e}"' for e in enum], '|'), ')']
  else:
    otype = schema.get('type', 'object')
    if otype in ('number', 'integer', 'string', 'boolean', 'null'):
      yield otype
    elif isinstance(otype, list):
      yield '('
      for i, t in enumerate(otype):
        if i > 0: yield '|'
        yield from process_type(path + [i], {**schema, 'type': t}, defs, type_to_def_name)
      yield ')'
    elif otype == 'array':
      items = schema.get('items')
      min_items = schema.get('minItems', 0)
      repetition = '*'
      if min_items == 1: repetition = '+'
      elif min_items > 1: print("min_items > 1 not supported yet", file=sys.stderr)
      yield from ['"["', "(", *process_type(path + ['[]'], items, defs, type_to_def_name), '","', ")", repetition, '"]"']
    else:
      assert otype == 'object', f"Unknown type {otype}: {schema}"
      if properties is not None:
        if additional_properties != False: print("Mix of properties and additionalProperties not supported yet", file=sys.stderr)
      
        required = schema.get('required', None) or []
        optional = [k for k in properties.keys() if k not in required]
        
        if schema.get('uniqueItems', False): print("uniqueItems not supported yet", file=sys.stderr)

        def gen_kv(k):
          return [get_ref(f'{k}_kv', [f'"\\"{k}\\":"', *process_type(path + [k], properties[k], defs, type_to_def_name)])]
        
        def get_opt_kv(k):
          # return get_ref(f'{k}_kv_opt', ["(", '","', gen_kv(k), ")?"])
          return ["(", '","', *gen_kv(k), ")?"]

        yield from ["{", *join_iter([t for k in required for t in gen_kv(k)], '","')]
        
        if len(optional) > 0:
          yield "("
          if len(required) > 0: yield from ['","', "("]

          def get_recursive_refs(ks, first_is_optional):
            [k, *rest] = ks
            if first_is_optional:
              yield from get_opt_kv(k)
            else:
              yield from gen_kv(k)
            if len(rest) > 0:
              yield get_ref(f'{k}_rest', [*get_recursive_refs(rest, first_is_optional=True)])
            
          for i in range(len(optional)):
            if i > 0: yield "|"
            yield from get_recursive_refs(optional[i:], first_is_optional=False)

          if len(required) > 0: yield ")"
          yield ")?"

        yield '"}"'
      elif additional_properties != False:
        yield '"{"'
        def gen_additional_properties(t):
          kv = get_ref('kv_', ["string", '":"', *process_type(path + ['$additionalProperties'], t, defs, type_to_def_name)])
          return ["(", kv, "(", '","', kv, ")?", ")?"]
        if isinstance(additional_properties, dict):
          yield from gen_additional_properties(additional_properties)
        else:
          yield from gen_additional_properties({})
        yield '"}"'
      else:
        print("No properties or additionalProperties for " + ' / '.join(path), file=sys.stderr)

def main(url: str, definition: Optional[str] = None):
  schema = get_schema(url)
  if definition is not None:
    schema = schema['definitions'][definition]
  elif 'properties' not in schema:
    print(f"Schema has no properties, please specify the name of a definition to process. Valid definitions: " + ', '.join(schema['definitions'].keys()), file=sys.stderr)
    return 1

  defs = {}
  print('\nroot ::= ' + ' '.join(process_type([url], schema, defs)) + '''
value  ::= object | array | string | number | boolean | null
object ::= "{" ( string ":" value ("," string ":" value)* )? "}"
array  ::= "[" ( value ("," value)* )? "]"
string ::= "\\"" (
  [^"\\\\] |
  "\\\\" (["\\\\/bfnrt] | "u" [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F]) # escapes
)* "\\""
boolean ::= "true" | "false"
null ::= "null"
integer ::= "-"? ([0-9] | [1-9] [0-9]*)
number ::= integer ("." [0-9]+)? ([eE] [-+]? [0-9]+)?
''' + '\n'.join(f'{k} ::= {" ".join(v)}' for k, v in defs.items()) + '\n')

if __name__ == "__main__":
  jsonargparse.CLI(main)
