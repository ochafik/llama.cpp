#!/usr/bin/env python
import pytest

# ensure grandparent path is in sys.path
from pathlib import Path
import sys

from unit.test_tool_call import TEST_TOOL
path = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(path))

import datetime
from utils import *

server: ServerProcess

TIMEOUT_SERVER_START = 15*60

@pytest.fixture(autouse=True)
def create_server():
    global server
    server = ServerPreset.tinyllama2()
    server.model_alias = "tinyllama-2"
    server.server_port = 8081
    server.n_slots = 1


@pytest.mark.parametrize("tools", [None, [], [TEST_TOOL]])
@pytest.mark.parametrize("template_name,nothink,expected_end", [
    ("deepseek-ai-DeepSeek-R1-Distill-Qwen-32B", False,  "<think>\n"),
    ("deepseek-ai-DeepSeek-R1-Distill-Qwen-32B", True, "<think>\n</think>"),

    ("Qwen-Qwen3-0.6B", False,  "<|im_start|>assistant\n"),
    ("Qwen-Qwen3-0.6B", True, "<|im_start|>assistant\n<think>\n\n</think>\n\n"),

    ("Qwen-QwQ-32B", False,  "<|im_start|>assistant\n<think>\n"),
    ("Qwen-QwQ-32B", True, "<|im_start|>assistant\n<think>\n</think>"),

    ("CohereForAI-c4ai-command-r7b-12-2024-tool_use-think", False,  "<|START_THINKING|>"),
    ("CohereForAI-c4ai-command-r7b-12-2024-tool_use-think", True, "<|START_THINKING|><|END_THINKING|>"),
])
def test_nothink(template_name: str, nothink: bool, expected_end: str, tools: list[dict]):
    global server
    server.jinja = True
    server.reasoning_format = 'nothink' if nothink else None
    server.chat_template_file = f'../../../models/templates/{template_name}.jinja'
    server.start(timeout_seconds=TIMEOUT_SERVER_START)

    res = server.make_request("POST", "/apply-template", data={
        "messages": [
            {"role": "user", "content": "What is today?"},
        ],
        "tools": tools,
    })
    assert res.status_code == 200
    prompt = res.body["prompt"]

    assert prompt.endswith(expected_end), f"Expected prompt to end with '{expected_end}', got '{prompt}'"


@pytest.mark.parametrize("tools", [None, [], [TEST_TOOL]])
@pytest.mark.parametrize("template_name,format", [
    ("meta-llama-Llama-3.3-70B-Instruct",    "%d %b %Y"),
    ("fireworks-ai-llama-3-firefunction-v2", "%b %d %Y"),
])
def test_date_inside_prompt(template_name: str, format: str, tools: list[dict]):
    global server
    server.jinja = True
    server.chat_template_file = f'../../../models/templates/{template_name}.jinja'
    server.start(timeout_seconds=TIMEOUT_SERVER_START)

    res = server.make_request("POST", "/apply-template", data={
        "messages": [
            {"role": "user", "content": "What is today?"},
        ],
        "tools": tools,
    })
    assert res.status_code == 200
    prompt = res.body["prompt"]

    today_str = datetime.date.today().strftime(format)
    assert today_str in prompt, f"Expected today's date ({today_str}) in content ({prompt})"
