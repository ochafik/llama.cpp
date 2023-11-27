# Generates a GBNF grammar from a JSON schema (https://www.schemastore.org/json/)
#
#   python grammars/from-json-schema.py https://json.schemastore.org/compile-commands.json
# 
# Pro-tip: Use projects such as typescript-json-schema to generate a JSON schema from a TypeScript type:
#
#   echo "export type Foo = { instruction: string, input?: string, output: string }" > foo.d.ts 
#   python grammars/from-json-schema.py <( npx typescript-json-schema --defaultProps --required foo.d.ts Foo )
import json, sys, requests, jsonargparse

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

def process_type(schema):
  ref = schema.get('$ref', None)
  if ref is not None:
    yield from process_type(get_schema(ref))
  else:
    otype = schema.get('type', 'object')
    if otype in ('number', 'integer', 'string', 'boolean'):
      yield otype
    elif otype == 'array':
      items = schema.get('items')
      min_items = schema.get('minItems', 0)
      repetition = '*'
      if min_items == 1: repetition = '+'
      elif min_items > 1: print("min_items > 1 not supported yet", file=sys.stderr)
      yield from ['"["', "(", *process_type(items), '","', ")", repetition, '"]"']
    else:
      assert otype == 'object', f"Unknown type {otype}: {schema}"
      properties = schema['properties']
      required = schema.get('required', None) or []
      optional = [k for k in properties.keys() if k not in required]
      
      if schema.get('uniqueItems', False): print("uniqueItems not supported yet", file=sys.stderr)

      def gen_kv(k): return [f'"\\"{k}\\":"', *process_type(properties[k])]
      
      yield '"{"'
      for i, k in enumerate(required):
        if i > 0: yield '","'
        yield from gen_kv(k)

      yield "("
      if len(required) > 0: yield from ['","', "("]

      for i in range(len(optional)):
        [first, *rest] = optional[i:]
        if i > 0: yield "|"
        yield from gen_kv(first)
        for other in rest: yield from ["(", '","', *gen_kv(other), ")?"]

      if len(required) > 0: yield ")"
      yield from [")?", '"}"']

def main(schema: str):
  schema = get_schema(schema)
  print('\nroot ::= ' + ' '.join(process_type(schema)) + '''
value  ::= object | array | string | number | boolean | "null"
object ::= "{" ( string ":" value ("," string ":" value)* )? "}"
array  ::= "[" ( value ("," value)* )? "]"
string ::= "\\"" (
  [^"\\\\] |
  "\\\\" (["\\\\/bfnrt] | "u" [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F]) # escapes
)* "\\""
boolean ::= "true" | "false"
integer ::= "-"? ([0-9] | [1-9] [0-9]*)
number ::= integer ("." [0-9]+)? ([eE] [-+]? [0-9]+)?
''')

if __name__ == "__main__":
  jsonargparse.CLI(main)
