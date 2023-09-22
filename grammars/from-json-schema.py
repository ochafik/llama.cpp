#
# python grammars/from-json-schema.py https://json.schemastore.org/compile-commands.json
# 
#
import json, sys, os, pathlib, re
from jsonargparse import CLI

refs = {}

def get_schema(url):
  if url not in refs:
    if url.startswith("https://"):
      import requests
      schema = requests.get(url).text
    else:
      with open(url, 'rt') as f:
        schema = f.read()
    schema = json.loads(schema)
    definitions = schema.get('definitions', None)
    if definitions is not None:
      for k, v in definitions.items():
        refs[f'#/definitions/{k}'] = v
    refs[url] = schema
  
  return refs[url]

def process_type(schema):
  ref = schema.get('$ref', None)
  if ref is not None:
    yield from process_type(get_schema(ref))
    return

  otype = schema.get('type', 'object')
  if otype in ('number', 'integer', 'string', 'boolean'):
    yield otype
    return

  if otype == 'array':
    items = schema.get('items')
    min_items = schema.get('minItems', 0)
    if min_items == 0:
      repetition = '*'
    elif min_items == 1:
      repetition = '+'
    else:
      print("min_items > 1 not supported yet", file=sys.stderr)
      repetition = '*'

    yield from ['"["', "(", *process_type(items), '","', ")", repetition, '"]"']
    return

  assert otype == 'object', f"Unknown type {otype}: {schema}"

  properties = schema['properties']
  required = schema.get('required', None) or []
  
  if schema.get('uniqueItems', False):
    print("uniqueItems not supported yet", file=sys.stderr)

  yield '"{"'
  for k in required:
    yield from [f'"\\"{k}\\":"', *process_type(properties[k]), '","']

  for k, v in properties.items():
    if k not in required:
      yield from ['(', f'"\\"{k}\\":"', *process_type(v), '","', ')', '?']

  yield '"}"'

def main(schema: str):
  schema = get_schema(schema)
  print('\nroot ::= ' + ' '.join(process_type(schema)) + '''

value  ::= object | array | string | number | ("true" | "false" | "null")

object ::= "{" ( string ":" value ("," string ":" value)* )? "}"

array  ::= "[" ( value ("," value)* )? "]"

string ::=
  "\\"" (
    [^"\\\\] |
    "\\\\" (["\\\\/bfnrt] | "u" [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F]) # escapes
  )* "\\""

integer ::= "-"? ([0-9] | [1-9] [0-9]*)

number ::= integer ("." [0-9]+)? ([eE] [-+]? [0-9]+)?
''')

if __name__ == "__main__":
  CLI(main)
