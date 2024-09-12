#!/usr/bin/env uv run
# /// script
# requires-python = ">=3.10"
# dependencies = [
#     "jinja2",
#     "huggingface_hub",
# ]
# ///
'''
  Executes a Jinja2 template (read from CLI arg) with a given context (read from JSON file as CLI arg)
  
  Examples:
    python ./tests/get-jinja-goldens.py
    python ./tests/run-jinja.py NousResearch/Hermes-2-Pro-Llama-3-8B  tests/chat/contexts/simple.json
    
    python ./tests/run-jinja.py tests/chat/templates/Meta-Llama-3.1-8B-Instruct.jinja tests/chat/contexts/simple.json
    python ./tests/run-jinja.py tests/chat/templates/Meta-Llama-3.1-8B-Instruct.jinja tests/chat/contexts/tooling.json

  This will automatically be run by tests/test-jinja.cpp to generate goldens against which the C++ jinja implementation will be tested.
  
  https://github.com/huggingface/transformers/blob/main/src/transformers/utils/chat_template_utils.py
'''

import datetime
import glob
from huggingface_hub import hf_hub_download
import json
import jinja2
import jinja2.ext
import re
# import requests

model_ids = [
    "NousResearch/Hermes-2-Pro-Llama-3-8B",
    "meetkai/functionary-medium-v3.2",
    "Qwen/Qwen2-VL-7B-Instruct",

    # Gated models:
    # "google/gemma-2-2b-it",
    # "mistralai/Mixtral-8x7B-Instruct-v0.1",
]

def raise_exception(message: str):
    raise ValueError(message)

def tojson(x, ensure_ascii=False, indent=None, separators=None, sort_keys=False):
    return json.dumps(x, ensure_ascii=ensure_ascii, indent=indent, separators=separators, sort_keys=sort_keys)

def strftime_now(format):
    return datetime.now().strftime(format)

def handle_chat_template(model_id, variant, template_src):
    print(f"# {model_id} @ {variant}")
    model_name = model_id.replace("/", "-")
    base_name = f'{model_name}-{variant}' if variant else model_name
    template_file = f'tests/chat/templates/{base_name}.jinja'
    with open(template_file, 'w') as f:
        f.write(template_src)
        
    print(f"- {template_file}")
    
    env = jinja2.Environment(
      trim_blocks=True,
      lstrip_blocks=True,
      extensions=[
        jinja2.ext.loopcontrols
      ])
    env.filters['tojson'] = tojson
    env.globals['raise_exception'] = raise_exception
    env.globals['strftime_now'] = strftime_now

    template = env.from_string(template_src)
    
    context_files = glob.glob('tests/chat/contexts/*.json')
    for context_file in context_files:
        context_name = context_file.split("/")[-1].replace(".json", "")
        with open(context_file, 'r') as f:
            context = json.load(f)
    
        output_file = f'tests/chat/goldens/{base_name}-{context_name}.txt'
        print(f"- {output_file}")
        try:
            output = template.render(**context)  
        except:
            # Some templates (e.g. Phi-3-medium-128k's) expect a non-null "content" key in each message.
            for message in context["messages"]:
                if message.get("content") is None:
                    message["content"] = ""

            try:
                output = template.render(**context)
            except Exception as e:
                print(f"  ERROR: {e}")
                output = f"ERROR: {e}"

        with open(output_file, 'w') as f:
            f.write(output)
            
    print()

def main():
    for model_id in model_ids:
        # response = requests.get(f"https://huggingface.co/{model_id}/resolve/main/tokenizer_config.json")
        # response.raise_for_status()
        # config_str = response.text
        with open(hf_hub_download(repo_id=model_id, filename="tokenizer_config.json")) as f:
            config_str = f.read()
               
        try: 
            config = json.loads(config_str)
        except json.JSONDecodeError as e:
            # Fix https://huggingface.co/NousResearch/Meta-Llama-3-8B-Instruct/blob/main/tokenizer_config.json
            # (Remove extra '}' near the end of the file)
            config = json.loads(re.sub(r'\}([\n\s]*\}[\n\s]*\],[\n\s]*"clean_up_tokenization_spaces")', r'\1', config_str))        

        chat_template = config['chat_template']
        if isinstance(chat_template, str):
            handle_chat_template(model_id, None, chat_template)
        else:
            for ct in chat_template:
                handle_chat_template(model_id, ct['name'], ct['template'])

if __name__ == '__main__':
    main()