#!/usr/bin/env python
'''
  Fetches the Jinja chat template and EOS tokens of a HuggingFace model.
  If a model has multiple chat templates, you can specify the variant name.

  Syntax:
    ./scripts/get_chat_template.py [--output-dir DIR] model_id [variant]

  Examples:
    ./scripts/get_chat_template.py --output-dir models/templates meta-llama/Llama-3.1-8B-Instruct
    ./scripts/get_chat_template.py --output-dir models/templates CohereForAI/c4ai-command-r-plus tool_use

  Without --output-dir, prints template to stdout.
  With --output-dir, saves both:
    - {model_id}.jinja (chat template)
    - {model_id}.metadata.json (model metadata including EOS tokens)
'''

import json
import re
import sys
import os


def fetch_json(model_id, filename):
    """Fetch a JSON file from HuggingFace, returns None if not found."""
    try:
        from huggingface_hub import hf_hub_download
        try:
            path = hf_hub_download(repo_id=model_id, filename=filename)
            with open(path, encoding="utf-8") as f:
                return json.load(f)
        except Exception:
            return None
    except ImportError:
        import requests
        assert re.match(r"^[\w.-]+/[\w.-]+$", model_id), f"Invalid model ID: {model_id}"
        response = requests.get(f"https://huggingface.co/{model_id}/resolve/main/{filename}")
        if response.status_code == 404:
            return None
        if response.status_code == 401:
            raise Exception('Access to this model is gated, please request access, authenticate with `huggingface-cli login` and make sure to run `pip install huggingface_hub`')
        response.raise_for_status()
        return response.json()


def get_tokenizer_config(model_id):
    """Fetch tokenizer_config.json and handle common issues."""
    try:
        from huggingface_hub import hf_hub_download
        with open(hf_hub_download(repo_id=model_id, filename="tokenizer_config.json"), encoding="utf-8") as f:
            config_str = f.read()
    except ImportError:
        import requests
        assert re.match(r"^[\w.-]+/[\w.-]+$", model_id), f"Invalid model ID: {model_id}"
        response = requests.get(f"https://huggingface.co/{model_id}/resolve/main/tokenizer_config.json")
        if response.status_code == 401:
            raise Exception('Access to this model is gated, please request access, authenticate with `huggingface-cli login` and make sure to run `pip install huggingface_hub`')
        response.raise_for_status()
        config_str = response.text

    try:
        return json.loads(config_str)
    except json.JSONDecodeError:
        # Fix https://huggingface.co/NousResearch/Meta-Llama-3-8B-Instruct/blob/main/tokenizer_config.json
        # (Remove extra '}' near the end of the file)
        return json.loads(re.sub(r'\}([\n\s]*\}[\n\s]*\],[\n\s]*"clean_up_tokenization_spaces")', r'\1', config_str))


def get_chat_template(model_id, variant=None):
    config = get_tokenizer_config(model_id)

    chat_template = config['chat_template']
    if isinstance(chat_template, str):
        return chat_template
    else:
        variants = {
            ct['name']: ct['template']
            for ct in chat_template
        }

        def format_variants():
            return ', '.join(f'"{v}"' for v in variants.keys())

        if variant is None:
            if 'default' not in variants:
                raise Exception(f'Please specify a chat template variant (one of {format_variants()})')
            variant = 'default'
            sys.stderr.write(f'Note: picked "default" chat template variant (out of {format_variants()})\n')
        elif variant not in variants:
            raise Exception(f"Variant {variant} not found in chat template (found {format_variants()})")

        return variants[variant]


def get_eos_tokens(model_id):
    """
    Fetch EOS tokens for a model by looking up eos_token_id in generation_config.json
    and resolving the token text from tokenizer_config.json's added_tokens_decoder.
    Returns a list of token strings.
    """
    # Get generation config for eos_token_id
    gen_config = fetch_json(model_id, "generation_config.json")
    if gen_config is None:
        sys.stderr.write(f"Warning: No generation_config.json found for {model_id}\n")
        return []

    eos_token_ids = gen_config.get("eos_token_id", [])
    if isinstance(eos_token_ids, int):
        eos_token_ids = [eos_token_ids]
    if not eos_token_ids:
        sys.stderr.write(f"Warning: No eos_token_id found in generation_config.json for {model_id}\n")
        return []

    # Get tokenizer config for added_tokens_decoder
    tokenizer_config = get_tokenizer_config(model_id)
    added_tokens_decoder = tokenizer_config.get("added_tokens_decoder", {})

    # Also check for eos_token directly in tokenizer config
    eos_token = tokenizer_config.get("eos_token")

    # Map token IDs to their text
    eos_tokens = []
    for token_id in eos_token_ids:
        token_id_str = str(token_id)
        if token_id_str in added_tokens_decoder:
            token_info = added_tokens_decoder[token_id_str]
            if isinstance(token_info, dict):
                token_text = token_info.get("content", token_info.get("text", f"<id:{token_id}>"))
            else:
                token_text = str(token_info)
            eos_tokens.append(token_text)
        else:
            sys.stderr.write(f"Warning: Token ID {token_id} not found in added_tokens_decoder for {model_id}\n")
            eos_tokens.append(f"<id:{token_id}>")

    # Add eos_token from tokenizer config if not already present
    if eos_token and isinstance(eos_token, str) and eos_token not in eos_tokens:
        eos_tokens.append(eos_token)

    return eos_tokens


def model_id_from_template_name(template_name):
    """
    Convert a template filename (without .jinja) to a HuggingFace model ID.
    E.g., 'meta-llama-Llama-3.1-8B-Instruct' -> 'meta-llama/Llama-3.1-8B-Instruct'
    """
    # Handle variant suffix (e.g., 'tool_use')
    variant = None
    for suffix in ['_tool_use', '-tool_use']:
        if template_name.endswith(suffix):
            variant = 'tool_use'
            template_name = template_name[:-len(suffix)]
            break

    # Split on first dash to separate org from model name
    # But some orgs have dashes in them (e.g., 'deepseek-ai')
    known_orgs = [
        'meta-llama', 'deepseek-ai', 'fireworks-ai', 'llama-cpp',
        'CohereForAI', 'meetkai', 'google', 'microsoft', 'mistralai',
        'NousResearch', 'Qwen', 'openai', 'THUDM', 'moonshotai',
        'ByteDance', 'NVIDIA', 'nvidia', 'MiniMax', 'LGAI-EXAONE'
    ]

    model_id = None
    for org in known_orgs:
        if template_name.startswith(org + '-'):
            model_name = template_name[len(org) + 1:]
            model_id = f"{org}/{model_name}"
            break

    if model_id is None:
        # Try splitting on first dash
        parts = template_name.split('-', 1)
        if len(parts) == 2:
            model_id = f"{parts[0]}/{parts[1]}"
        else:
            model_id = template_name

    return model_id, variant


def main(args):
    output_dir = None

    # Parse flags
    while args and args[0].startswith('--'):
        flag = args.pop(0)
        if flag.startswith('--output-dir='):
            output_dir = flag.split('=', 1)[1]
        elif flag == '--output-dir':
            output_dir = args.pop(0)
        else:
            raise ValueError(f"Unknown flag: {flag}")

    if len(args) < 1:
        raise ValueError("Please provide a model ID and an optional variant name")
    model_id = args[0]
    variant = None if len(args) < 2 else args[1]

    # Build filename base
    safe_name = model_id.replace('/', '-')
    if variant:
        safe_name += f"-{variant}"

    # Create output directory if needed
    if output_dir:
        os.makedirs(output_dir, exist_ok=True)

    # Fetch and save/print template
    try:
        template = get_chat_template(model_id, variant)
        if output_dir:
            jinja_path = os.path.join(output_dir, f"{safe_name}.jinja")
            with open(jinja_path, 'w', encoding='utf-8') as f:
                f.write(template)
            sys.stderr.write(f"Saved template to: {jinja_path}\n")
        else:
            sys.stdout.write(template)
    except KeyError as e:
        if 'chat_template' in str(e):
            sys.stderr.write(f"Warning: No chat_template found for {model_id}\n")
        else:
            raise

    # Fetch and save metadata (only when output_dir is specified)
    if output_dir:
        eos_tokens = get_eos_tokens(model_id)
        metadata = {"model_id": model_id}
        if eos_tokens:
            metadata["eos_tokens"] = eos_tokens
        metadata_path = os.path.join(output_dir, f"{safe_name}.metadata.json")
        with open(metadata_path, 'w', encoding='utf-8') as f:
            json.dump(metadata, f, indent=2)
        sys.stderr.write(f"Saved metadata to: {metadata_path}\n")


if __name__ == '__main__':
    main(sys.argv[1:])
