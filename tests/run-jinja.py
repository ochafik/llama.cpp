#!/usr/bin/env uv run
# /// script
# requires-python = ">=3.10"
# dependencies = [
#     "jinja2",
# ]
# ///
'''
  Executes a Jinja2 template (read from CLI arg) with a given context (read from JSON file as CLI arg)
  
  Examples:
    python ./tests/run-jinja.py tests/chat/templates/Meta-Llama-3.1-8B-Instruct.jinja tests/chat/contexts/simple.json
    python ./tests/run-jinja.py tests/chat/templates/Meta-Llama-3.1-8B-Instruct.jinja tests/chat/contexts/tooling.json

  This will automatically be run by tests/test-jinja.cpp to generate goldens against which the C++ jinja implementation will be tested.
  
  https://github.com/huggingface/transformers/blob/main/src/transformers/utils/chat_template_utils.py
'''

import sys
import json
import datetime
from jinja2 import Environment, FileSystemLoader
import jinja2.ext

def raise_exception(message: str):
    raise ValueError(message)

def tojson(x, ensure_ascii=False, indent=None, separators=None, sort_keys=False):
    return json.dumps(x, ensure_ascii=ensure_ascii, indent=indent, separators=separators, sort_keys=sort_keys)

def strftime_now(format):
    return datetime.now().strftime(format)

def main():
    if len(sys.argv) != 3:
        print("Usage: {} <template> <context>".format(sys.argv[0]))
        sys.exit(1)

    template = sys.argv[1]
    context = sys.argv[2]

    with open(context, 'r') as f:
        context = json.load(f)

    env = Environment(
      trim_blocks=True,
      lstrip_blocks=True,
      extensions=[
        # AssistantTracker,
        jinja2.ext.loopcontrols
      ],
      loader=FileSystemLoader('.'))
    
    env.filters['tojson'] = tojson
    env.globals['raise_exception'] = raise_exception
    env.globals['strftime_now'] = strftime_now

    template = env.get_template(template)
    
    # Some templates (e.g. Phi-3-medium-128k's) expect a non-null "content" key in each message.
    for message in context["messages"]:
      if message.get("content") is None:
        message["content"] = ""

    # print(json.dumps(context, indent=2))
    try:
      print(template.render(
        # messages=context["messages"],
        **context
      ))
    except Exception as e:
      # Print stack trace
      print(f'ERROR: {e}')

if __name__ == '__main__':
    main()