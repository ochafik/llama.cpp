import argparse
from statistics import mean, median
import pytest

# ensure grandparent path is in sys.path
from pathlib import Path
import sys
path = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(path))

from utils import *

server: ServerProcess

TIMEOUT_SERVER_START = 15*60
TIMEOUT_HTTP_REQUEST = 60

@pytest.fixture(autouse=True)
def create_server():
    global server
    server = ServerPreset.tinyllama2()
    server.model_alias = "tinyllama-2-tool-call"
    server.server_port = 8081


TEST_TOOL = {
    "type":"function",
    "function": {
        "name": "test",
        "description": "",
        "parameters": {
            "type": "object",
            "properties": {
                "success": {"type": "boolean", "const": True},
            },
            "required": ["success"]
        }
    }
}

PYTHON_TOOL = {
    "type": "function",
    "function": {
        "name": "python",
        "description": "Runs code in an ipython interpreter and returns the result of the execution after 60 seconds.",
        "parameters": {
            "type": "object",
            "properties": {
                "code": {
                    "type": "string",
                    "description": "The code to run in the ipython interpreter."
                }
            },
            "required": ["code"]
        }
    }
}

WEATHER_TOOL = {
  "type":"function",
  "function":{
    "name":"get_current_weather",
    "description":"Get the current weather in a given location",
    "parameters":{
      "type":"object",
      "properties":{
        "location":{
          "type":"string",
          "description":"The city and country/state, e.g. 'San Francisco, CA', or 'Paris, France'"
        }
      },
      "required":["location"]
    }
  }
}


def do_test_completion_with_required_tool_tiny(tool: dict, argument_key: str | None, **kwargs):
    global server
    res = server.make_request("POST", "/v1/chat/completions", data={
        "max_tokens": n_predict,
        "messages": [
            {"role": "system", "content": "You are a coding assistant."},
            {"role": "user", "content": "Write an example"},
        ],
        "tool_choice": "required",
        "tools": [tool],
        "parallel_tool_calls": False,
        **kwargs,
    })
    assert res.status_code == 200, f"Expected status code 200, got {res.status_code}"
    choice = res.body["choices"][0]
    tool_calls = choice["message"].get("tool_calls")
    assert tool_calls and len(tool_calls) == 1, f'Expected 1 tool call in {choice["message"]}'
    tool_call = tool_calls[0]
    assert choice["message"].get("content") in (None, ""), f'Expected no content in {choice["message"]}'
    expected_function_name = "python" if tool["type"] == "code_interpreter" else tool["function"]["name"]
    assert expected_function_name == tool_call["function"]["name"]
    actual_arguments = tool_call["function"]["arguments"]
    assert isinstance(actual_arguments, str)
    if argument_key is not None:
        actual_arguments = json.loads(actual_arguments)
        assert argument_key in actual_arguments, f"tool arguments: {json.dumps(actual_arguments)}, expected: {argument_key}"


@pytest.mark.parametrize("template_name,tool,argument_key", [
    ("google-gemma-2-2b-it",                          TEST_TOOL,            "success"),
    ("meta-llama-Llama-3.3-70B-Instruct",             TEST_TOOL,            "success"),
    ("meta-llama-Llama-3.3-70B-Instruct",             PYTHON_TOOL,          "code"),
])
def test_completion_with_required_tool_tiny_fast(template_name: str, tool: dict, argument_key: str | None):
    global server
    n_predict = 512
    # server = ServerPreset.stories15m_moe()
    server.jinja = True
    server.n_predict = n_predict
    server.chat_template_file = f'../../../models/templates/{template_name}.jinja'
    server.start(timeout_seconds=TIMEOUT_SERVER_START)
    do_test_completion_with_required_tool_tiny(tool, argument_key, temperature=0.0, top_k=1, top_p=1.0)


@pytest.mark.slow
@pytest.mark.parametrize("template_name,tool,argument_key", [
    ("meta-llama-Llama-3.1-8B-Instruct",              TEST_TOOL,            "success"),
    ("meta-llama-Llama-3.1-8B-Instruct",              PYTHON_TOOL,          "code"),
    ("meetkai-functionary-medium-v3.1",               TEST_TOOL,            "success"),
    ("meetkai-functionary-medium-v3.1",               PYTHON_TOOL,          "code"),
    ("meetkai-functionary-medium-v3.2",               TEST_TOOL,            "success"),
    ("meetkai-functionary-medium-v3.2",               PYTHON_TOOL,          "code"),
    ("NousResearch-Hermes-2-Pro-Llama-3-8B-tool_use", TEST_TOOL,            "success"),
    ("NousResearch-Hermes-2-Pro-Llama-3-8B-tool_use", PYTHON_TOOL,          "code"),
    ("meta-llama-Llama-3.2-3B-Instruct",              TEST_TOOL,            "success"),
    ("meta-llama-Llama-3.2-3B-Instruct",              PYTHON_TOOL,          "code"),
    ("mistralai-Mistral-Nemo-Instruct-2407",          TEST_TOOL,            "success"),
    ("mistralai-Mistral-Nemo-Instruct-2407",          PYTHON_TOOL,          "code"),
    ("NousResearch-Hermes-3-Llama-3.1-8B-tool_use",   TEST_TOOL,            "success"),
    ("NousResearch-Hermes-3-Llama-3.1-8B-tool_use",   PYTHON_TOOL,          "code"),
    ("deepseek-ai-DeepSeek-R1-Distill-Llama-8B",      TEST_TOOL,            "success"),
    ("deepseek-ai-DeepSeek-R1-Distill-Llama-8B",      PYTHON_TOOL,          "code"),
    ("fireworks-ai-llama-3-firefunction-v2",          TEST_TOOL,            "success"),
    ("fireworks-ai-llama-3-firefunction-v2",          PYTHON_TOOL,          "code"),
])
def test_completion_with_required_tool_tiny_slow(template_name: str, tool: dict, argument_key: str | None):
    global server
    n_predict = 512
    # server = ServerPreset.stories15m_moe()
    server.jinja = True
    server.n_predict = n_predict
    server.chat_template_file = f'../../../models/templates/{template_name}.jinja'
    server.start(timeout_seconds=TIMEOUT_SERVER_START)
    do_test_completion_with_required_tool_tiny(template_name, tool, argument_key)


@pytest.mark.slow
@pytest.mark.parametrize("tool,argument_key,hf_repo,template_override", [
    (TEST_TOOL,    "success",  "bartowski/Meta-Llama-3.1-8B-Instruct-GGUF", None),
    (PYTHON_TOOL,  "code",     "bartowski/Meta-Llama-3.1-8B-Instruct-GGUF", None),
    (PYTHON_TOOL,  "code",     "bartowski/Meta-Llama-3.1-8B-Instruct-GGUF", "chatml"),

    # Note: gemma-2-2b-it knows itself as "model", not "assistant", so we don't test the ill-suited chatml on it.
    (TEST_TOOL,    "success",  "bartowski/gemma-2-2b-it-GGUF",              None),
    (PYTHON_TOOL,  "code",     "bartowski/gemma-2-2b-it-GGUF",              None),

    (TEST_TOOL,    "success",  "bartowski/Phi-3.5-mini-instruct-GGUF",      None),
    (PYTHON_TOOL,  "code",     "bartowski/Phi-3.5-mini-instruct-GGUF",      None),
    (PYTHON_TOOL,  "code",     "bartowski/Phi-3.5-mini-instruct-GGUF",      "chatml"),

    (TEST_TOOL,    "success",  "bartowski/Qwen2.5-7B-Instruct-GGUF",        None),
    (PYTHON_TOOL,  "code",     "bartowski/Qwen2.5-7B-Instruct-GGUF",        None),
    (PYTHON_TOOL,  "code",     "bartowski/Qwen2.5-7B-Instruct-GGUF",        "chatml"),

    (TEST_TOOL,    "success",  "bartowski/Hermes-2-Pro-Llama-3-8B-GGUF", ("NousResearch/Hermes-2-Pro-Llama-3-8B", "tool_use")),
    (PYTHON_TOOL,  "code",     "bartowski/Hermes-2-Pro-Llama-3-8B-GGUF", ("NousResearch/Hermes-2-Pro-Llama-3-8B", "tool_use")),
    # (PYTHON_TOOL,  "code",     "bartowski/Hermes-2-Pro-Llama-3-8B-GGUF", "chatml"),

    (TEST_TOOL,    "success",  "bartowski/Hermes-3-Llama-3.1-8B-GGUF",   ("NousResearch/Hermes-3-Llama-3.1-8B", "tool_use")),
    (PYTHON_TOOL,  "code",     "bartowski/Hermes-3-Llama-3.1-8B-GGUF",   ("NousResearch/Hermes-3-Llama-3.1-8B", "tool_use")),
    # (PYTHON_TOOL,  "code",     "bartowski/Hermes-3-Llama-3.1-8B-GGUF",   "chatml"),

    (TEST_TOOL,    "success",  "bartowski/Mistral-Nemo-Instruct-2407-GGUF", None),
    (PYTHON_TOOL,  "code",     "bartowski/Mistral-Nemo-Instruct-2407-GGUF", None),
    (PYTHON_TOOL,  "code",     "bartowski/Mistral-Nemo-Instruct-2407-GGUF", "chatml"),

    (TEST_TOOL,    "success",  "bartowski/functionary-small-v3.2-GGUF",       ("meetkai/functionary-medium-v3.2", None)),
    (PYTHON_TOOL,  "code",     "bartowski/functionary-small-v3.2-GGUF",       ("meetkai/functionary-medium-v3.2", None)),
    (PYTHON_TOOL,  "code",     "bartowski/functionary-small-v3.2-GGUF",       "chatml"),

    (TEST_TOOL,    "success",  "bartowski/Llama-3.2-3B-Instruct-GGUF",      ("meta-llama/Llama-3.2-3B-Instruct", None)),
    (PYTHON_TOOL,  "code",     "bartowski/Llama-3.2-3B-Instruct-GGUF",      ("meta-llama/Llama-3.2-3B-Instruct", None)),
    (PYTHON_TOOL,  "code",     "bartowski/Llama-3.2-3B-Instruct-GGUF",      "chatml"),

    (TEST_TOOL,    "success",  "bartowski/Llama-3.2-1B-Instruct-GGUF",      ("meta-llama/Llama-3.2-3B-Instruct", None)),
    (PYTHON_TOOL,  "code",     "bartowski/Llama-3.2-1B-Instruct-GGUF",      ("meta-llama/Llama-3.2-3B-Instruct", None)),
    # (PYTHON_TOOL,  "code",     "bartowski/Llama-3.2-1B-Instruct-GGUF",      "chatml"),
    # TODO: fix these
    # (TEST_TOOL,    "success",  "bartowski/DeepSeek-R1-Distill-Qwen-7B-GGUF", None),
    # (PYTHON_TOOL,  "code",     "bartowski/DeepSeek-R1-Distill-Qwen-7B-GGUF", None),
])
def test_completion_with_required_tool_real_model(tool: dict, argument_key: str | None, hf_repo: str, template_override: str | Tuple[str, str | None] | None):
    global server
    n_predict = 512
    server.n_slots = 1
    server.jinja = True
    server.n_ctx = 8192
    server.n_predict = n_predict
    server.model_hf_repo = hf_repo
    server.model_hf_file = None
    if isinstance(template_override, tuple):
        (template_hf_repo, template_variant) = template_override
        server.chat_template_file = f"../../../models/templates/{template_hf_repo.replace('/', '-') + ('-' + template_variant if template_variant else '')}.jinja"
        assert os.path.exists(server.chat_template_file), f"Template file {server.chat_template_file} does not exist. Run `python scripts/get_chat_template.py {template_hf_repo} {template_variant} > {server.chat_template_file}` to download the template."
    elif isinstance(template_override, str):
        server.chat_template = template_override
    server.start(timeout_seconds=TIMEOUT_SERVER_START)
    res = server.make_request("POST", "/v1/chat/completions", data={
        "max_tokens": n_predict,
        "messages": [
            {"role": "system", "content": "You are a coding assistant."},
            {"role": "user", "content": "Write an example"},
        ],
        "tool_choice": "required",
        "tools": [tool],
        "parallel_tool_calls": False,
        "temperature": 0.0,
        "top_k": 1,
        "top_p": 1.0,
    }, timeout=TIMEOUT_HTTP_REQUEST)
    assert res.status_code == 200, f"Expected status code 200, got {res.status_code}"
    choice = res.body["choices"][0]
    tool_calls = choice["message"].get("tool_calls")
    assert tool_calls and len(tool_calls) == 1, f'Expected 1 tool call in {choice["message"]}'
    tool_call = tool_calls[0]
    assert choice["message"].get("content") in (None, ""), f'Expected no content in {choice["message"]}'
    expected_function_name = "python" if tool["type"] == "code_interpreter" else tool["function"]["name"]
    assert expected_function_name == tool_call["function"]["name"]
    actual_arguments = tool_call["function"]["arguments"]
    assert isinstance(actual_arguments, str)
    if argument_key is not None:
        actual_arguments = json.loads(actual_arguments)
        assert argument_key in actual_arguments, f"tool arguments: {json.dumps(actual_arguments)}, expected: {argument_key}"


def do_test_completion_without_tool_call(template_name: str, n_predict: int, tools: list[dict], tool_choice: str | None, **kwargs):
    global server
    res = server.make_request("POST", "/v1/chat/completions", data={
        "max_tokens": n_predict,
        "messages": [
            {"role": "system", "content": "You are a coding assistant."},
            {"role": "user", "content": "say hello world with python"},
        ],
        "tools": tools if tools else None,
        "tool_choice": tool_choice,
        **kwargs,
    }, timeout=TIMEOUT_HTTP_REQUEST)
    assert res.status_code == 200, f"Expected status code 200, got {res.status_code}"
    choice = res.body["choices"][0]
    assert choice["message"].get("tool_calls") is None, f'Expected no tool call in {choice["message"]}'


@pytest.mark.parametrize("template_name,n_predict,tools,tool_choice", [
    ("meta-llama-Llama-3.3-70B-Instruct",         128, [],            None),
    ("meta-llama-Llama-3.3-70B-Instruct",         128, [TEST_TOOL],   None),
    ("meta-llama-Llama-3.3-70B-Instruct",         128, [PYTHON_TOOL], 'none'),
])
def test_completion_without_tool_call_fast(template_name: str, n_predict: int, tools: list[dict], tool_choice: str | None):
    global server
    server.jinja = True
    server.n_predict = n_predict
    server.chat_template_file = f'../../../models/templates/{template_name}.jinja'
    server.start(timeout_seconds=TIMEOUT_SERVER_START)
    do_test_completion_without_tool_call(template_name, n_predict, tools, tool_choice)


@pytest.mark.slow
@pytest.mark.parametrize("template_name,n_predict,tools,tool_choice", [
    ("meetkai-functionary-medium-v3.2",               256, [],            None),
    ("meetkai-functionary-medium-v3.2",               256, [TEST_TOOL],   None),
    ("meetkai-functionary-medium-v3.2",               256, [PYTHON_TOOL], 'none'),
    ("meetkai-functionary-medium-v3.1",               256, [],            None),
    ("meetkai-functionary-medium-v3.1",               256, [TEST_TOOL],   None),
    ("meetkai-functionary-medium-v3.1",               256, [PYTHON_TOOL], 'none'),
    ("meta-llama-Llama-3.2-3B-Instruct",              256, [],            None),
    ("meta-llama-Llama-3.2-3B-Instruct",              256, [TEST_TOOL],   None),
    ("meta-llama-Llama-3.2-3B-Instruct",              256, [PYTHON_TOOL], 'none'),
])
def test_completion_without_tool_call_slow(template_name: str, n_predict: int, tools: list[dict], tool_choice: str | None):
    global server
    server.jinja = True
    server.n_predict = n_predict
    server.chat_template_file = f'../../../models/templates/{template_name}.jinja'
    server.start(timeout_seconds=TIMEOUT_SERVER_START)
    do_test_completion_without_tool_call(template_name, n_predict, tools, tool_choice)


def do_test_weather(**kwargs):
    global server
    res = server.make_request("POST", "/v1/chat/completions", data={
        "max_tokens": n_predict,
        "messages": [
            {"role": "system", "content": "You are a chatbot that uses tools/functions. Dont overthink things."},
            {"role": "user", "content": "What is the weather in Istanbul?"},
        ],
        "tools": [WEATHER_TOOL],
        **kwargs,
    }, timeout=TIMEOUT_HTTP_REQUEST)
    assert res.status_code == 200, f"Expected status code 200, got {res.status_code}"
    choice = res.body["choices"][0]
    tool_calls = choice["message"].get("tool_calls")
    assert tool_calls and len(tool_calls) == 1, f'Expected 1 tool call in {choice["message"]}'
    tool_call = tool_calls[0]
    assert choice["message"].get("content") in (None, ""), f'Expected no content in {choice["message"]}'
    assert tool_call["function"]["name"] == WEATHER_TOOL["function"]["name"]
    actual_arguments = json.loads(tool_call["function"]["arguments"])
    assert 'location' in actual_arguments, f"location not found in {json.dumps(actual_arguments)}"
    location = actual_arguments["location"]
    assert isinstance(location, str), f"Expected location to be a string, got {type(location)}: {json.dumps(location)}"
    assert re.match('^Istanbul(, (TR|Turkey|Türkiye))?$', location), f'Expected Istanbul for location, got {location}'


@pytest.mark.slow
@pytest.mark.parametrize("hf_repo,template_override", [
    ("bartowski/Meta-Llama-3.1-8B-Instruct-GGUF", None),
    ("bartowski/Meta-Llama-3.1-8B-Instruct-GGUF", "chatml"),

    ("bartowski/Phi-3.5-mini-instruct-GGUF",      None),
    ("bartowski/Phi-3.5-mini-instruct-GGUF",      "chatml"),

    ("bartowski/Qwen2.5-7B-Instruct-GGUF",        None),
    ("bartowski/Qwen2.5-7B-Instruct-GGUF",        "chatml"),

    ("bartowski/Hermes-2-Pro-Llama-3-8B-GGUF",    ("NousResearch/Hermes-2-Pro-Llama-3-8B", "tool_use")),
    ("bartowski/Hermes-2-Pro-Llama-3-8B-GGUF",    "chatml"),

    ("bartowski/Hermes-3-Llama-3.1-8B-GGUF",      ("NousResearch/Hermes-3-Llama-3.1-8B", "tool_use")),
    ("bartowski/Hermes-3-Llama-3.1-8B-GGUF",      "chatml"),

    ("bartowski/Mistral-Nemo-Instruct-2407-GGUF", None),
    ("bartowski/Mistral-Nemo-Instruct-2407-GGUF", "chatml"),

    ("bartowski/functionary-small-v3.2-GGUF:Q8_0",       ("meetkai/functionary-medium-v3.2", None)),
    ("bartowski/functionary-small-v3.2-GGUF:Q8_0",       "chatml"),

    ("bartowski/Llama-3.2-3B-Instruct-GGUF",      ("meta-llama/Llama-3.2-3B-Instruct", None)),
    ("bartowski/Llama-3.2-3B-Instruct-GGUF",      "chatml"),

    ("bartowski/c4ai-command-r7b-12-2024-GGUF:Q6_K_L",   ("CohereForAI/c4ai-command-r7b-12-2024", "tool_use")),

    ("bartowski/DeepSeek-R1-Distill-Qwen-7B-GGUF", None),

    # Note: gemma-2-2b-it knows itself as "model", not "assistant", so we don't test the ill-suited chatml on it.
    ("bartowski/gemma-2-2b-it-GGUF",              None),

    # ("bartowski/Llama-3.2-1B-Instruct-GGUF", ("meta-llama/Llama-3.2-3B-Instruct", None)),
])
def test_weather(hf_repo: str, template_override: str | Tuple[str, str | None] | None):
    global server
    n_predict = 512
    server.n_slots = 1
    server.jinja = True
    server.n_ctx = 8192
    server.n_predict = n_predict
    server.model_hf_repo = hf_repo
    server.model_hf_file = None
    if isinstance(template_override, tuple):
        (template_hf_repo, template_variant) = template_override
        server.chat_template_file = f"../../../models/templates/{template_hf_repo.replace('/', '-') + ('-' + template_variant if template_variant else '')}.jinja"
        assert os.path.exists(server.chat_template_file), f"Template file {server.chat_template_file} does not exist. Run `python scripts/get_chat_template.py {template_hf_repo} {template_variant} > {server.chat_template_file}` to download the template."
    elif isinstance(template_override, str):
        server.chat_template = template_override
    server.start(timeout_seconds=TIMEOUT_SERVER_START)
    do_test_weather()


def do_test_calc_result(result_override: str | None, n_predict: int, **kwargs):
    global server
    res = server.make_request("POST", "/v1/chat/completions", data={
        "max_tokens": n_predict,
        "messages": [
            {"role": "system", "content": "You are a chatbot that uses tools/functions. Dont overthink things, and provide very concise answers. Do not explain your reasoning to the user. Provide any numerical values back to the user with at most two decimals."},
            {"role": "user", "content": "What's the y coordinate of a point on the unit sphere at angle 30 degrees?"},
            {
                "role": "assistant",
                "content": None,
                "tool_calls": [
                    {
                        "id": "call_6789",
                        "type": "function",
                        "function": {
                            "name": "calculate",
                            "arguments": "{\"expression\":\"sin(30 * pi / 180)\"}"
                        }
                    }
                ]
            },
            {
                "role": "tool",
                "name": "calculate",
                "content": 0.55644242476,
                "tool_call_id": "call_6789"
            }
        ],
        "tools": [
            {
                "type":"function",
                "function":{
                    "name":"calculate",
                    "description":"A calculator function that computes values of arithmetic expressions in the Python syntax",
                    "parameters":{
                        "type":"object",
                        "properties":{
                            "expression":{
                            "type":"string",
                            "description":"An arithmetic expression to compute the value of (Python syntad, assuming all floats)"
                            }
                        },
                        "required":["expression"]
                    }
                }
            }
        ],
        **kwargs,
    }, timeout=TIMEOUT_HTTP_REQUEST)
    assert res.status_code == 200, f"Expected status code 200, got {res.status_code}"
    choice = res.body["choices"][0]
    tool_calls = choice["message"].get("tool_calls")
    assert tool_calls is None, f'Expected no tool call in {choice["message"]}'
    content = choice["message"].get("content")
    assert content is not None, f'Expected content in {choice["message"]}'
    if result_override is not None:
        assert re.match(result_override, content), f'Expected {result_override}, got {content}'
    else:
        assert re.match('^[\\s\\S]*?((That\'s|\\bis) (approximately )?)?\\b0\\.(5\\b|56\\b|556)', content), \
            f'Expected something like "The y coordinate is 0.56.", got {content}'


@pytest.mark.slow
@pytest.mark.parametrize("result_override,n_predict,hf_repo,template_override", [
    (None,                                           128,  "bartowski/Phi-3.5-mini-instruct-GGUF",       "chatml"),
    (None,                                           128,  "bartowski/Qwen2.5-7B-Instruct-GGUF",         None),
    (None,                                           128,  "bartowski/Qwen2.5-7B-Instruct-GGUF",         "chatml"),
    (None,                                           128,  "bartowski/Hermes-2-Pro-Llama-3-8B-GGUF",     ("NousResearch/Hermes-2-Pro-Llama-3-8B", "tool_use")),
    (None,                                           128,  "bartowski/Hermes-3-Llama-3.1-8B-GGUF",       ("NousResearch/Hermes-3-Llama-3.1-8B", "tool_use")),
    (None,                                           128,  "bartowski/functionary-small-v3.2-GGUF:Q8_0",        ("meetkai/functionary-medium-v3.2", None)),
    (None,                                           128,  "bartowski/Meta-Llama-3.1-8B-Instruct-GGUF",  None),
    (None,                                           128,  "bartowski/Mistral-Nemo-Instruct-2407-GGUF",  None),
    ("^> 0.56$",                                     128,  "bartowski/Mistral-Nemo-Instruct-2407-GGUF",  "chatml"),
    (None,                                           128,  "bartowski/Phi-3.5-mini-instruct-GGUF",       None),

    # TODO: fix these (wrong results, either didn't respect decimal instruction or got wrong value)
    ("^The y-coordinate [\\s\\S]*?\\*\\*0.5\\*\\*",  8192, "bartowski/DeepSeek-R1-Distill-Qwen-7B-GGUF", None),
    ("[\\s\\S]*?\\*\\*0\\.5\\*\\*",                  8192, "bartowski/DeepSeek-R1-Distill-Qwen-7B-GGUF", ("llama-cpp-deepseek-r1", None)),
])
def test_calc_result(result_override: str | None, n_predict: int, hf_repo: str, template_override: str | Tuple[str, str | None] | None):
    global server
    # n_predict = 512
    server.n_slots = 1
    server.jinja = True
    server.n_ctx = 8192 * 2
    server.n_predict = n_predict
    server.model_hf_repo = hf_repo
    server.model_hf_file = None
    if isinstance(template_override, tuple):
        (template_hf_repo, template_variant) = template_override
        server.chat_template_file = f"../../../models/templates/{template_hf_repo.replace('/', '-') + ('-' + template_variant if template_variant else '')}.jinja"
        assert os.path.exists(server.chat_template_file), f"Template file {server.chat_template_file} does not exist. Run `python scripts/get_chat_template.py {template_hf_repo} {template_variant} > {server.chat_template_file}` to download the template."
    elif isinstance(template_override, str):
        server.chat_template = template_override
    server.start(timeout_seconds=TIMEOUT_SERVER_START)
    do_test_calc_result(result_override, n_predict)


@pytest.mark.slow
@pytest.mark.parametrize("n_predict,reasoning_format,expect_content,expect_reasoning_content,hf_repo,template_override", [
    (128, 'deepseek',  "^The sum of 102 and 7 is 109.*",                        None,                                          "bartowski/Phi-3.5-mini-instruct-GGUF",       None),
    (128,  None,        "^The sum of 102 and 7 is 109.*",                       None,                                          "bartowski/Phi-3.5-mini-instruct-GGUF",       None),

    (1024, 'deepseek',  "To find the sum of.*",                                 "I need to calculate the sum of 102 and 7.*",  "bartowski/DeepSeek-R1-Distill-Qwen-7B-GGUF", None),
    (1024, 'none',      "<think>\n?I need[\\s\\S]*?</think>\n?To find.*",       None,                                          "bartowski/DeepSeek-R1-Distill-Qwen-7B-GGUF", None),

    (1024, 'deepseek',  "To find the sum of.*",                                 "First, I [\\s\\S]*",                          "bartowski/DeepSeek-R1-Distill-Qwen-7B-GGUF", ("llama-cpp-deepseek-r1", None)),
])
def test_thoughts(n_predict: int, reasoning_format: Literal['deepseek', 'none'] | None, expect_content: str | None, expect_reasoning_content: str | None, hf_repo: str, template_override: str | Tuple[str, str | None] | None):
    global server
    server.n_slots = 1
    server.reasoning_format = reasoning_format
    server.jinja = True
    server.n_ctx = 8192 * 2
    server.n_predict = n_predict
    server.model_hf_repo = hf_repo
    server.model_hf_file = None
    if isinstance(template_override, tuple):
        (template_hf_repo, template_variant) = template_override
        server.chat_template_file = f"../../../models/templates/{template_hf_repo.replace('/', '-') + ('-' + template_variant if template_variant else '')}.jinja"
        assert os.path.exists(server.chat_template_file), f"Template file {server.chat_template_file} does not exist. Run `python scripts/get_chat_template.py {template_hf_repo} {template_variant} > {server.chat_template_file}` to download the template."
    elif isinstance(template_override, str):
        server.chat_template = template_override
    server.start(timeout_seconds=TIMEOUT_SERVER_START)
    res = server.make_request("POST", "/v1/chat/completions", data={
        "max_tokens": n_predict,
        "messages": [
            {"role": "user", "content": "What's the sum of 102 and 7?"},
        ]
    }, timeout=TIMEOUT_HTTP_REQUEST)
    assert res.status_code == 200, f"Expected status code 200, got {res.status_code}"
    choice = res.body["choices"][0]
    assert choice["message"].get("tool_calls") is None, f'Expected no tool call in {choice["message"]}'

    content = choice["message"].get("content")
    if expect_content is None:
        assert choice["message"].get("content") in (None, ""), f'Expected no content in {choice["message"]}'
    else:
        assert re.match(expect_content, content), f'Expected {expect_content}, got {content}'

    reasoning_content = choice["message"].get("reasoning_content")
    if expect_reasoning_content is None:
        assert reasoning_content is None, f'Expected no reasoning content in {choice["message"]}'
    else:
        assert re.match(expect_reasoning_content, reasoning_content), f'Expected {expect_reasoning_content}, got {reasoning_content}'


def do_test_hello_world(expected_arguments_override: str | None, **kwargs):
    global server
    res = server.make_request("POST", "/v1/chat/completions", data={
        "max_tokens": 256,
        "messages": [
            {"role": "system", "content": "You are a coding assistant."},
            {"role": "user", "content": "say hello world with python"},
        ],
        "tools": [PYTHON_TOOL],
        **kwargs,
    }, timeout=TIMEOUT_HTTP_REQUEST)
    assert res.status_code == 200, f"Expected status code 200, got {res.status_code}"
    choice = res.body["choices"][0]
    tool_calls = choice["message"].get("tool_calls")
    assert tool_calls and len(tool_calls) == 1, f'Expected 1 tool call in {choice["message"]}'
    tool_call = tool_calls[0]
    assert choice["message"].get("content") in (None, ""), f'Expected no content in {choice["message"]}'
    assert tool_call["function"]["name"] == PYTHON_TOOL["function"]["name"]
    actual_arguments = tool_call["function"]["arguments"]
    if expected_arguments_override is not None:
        assert actual_arguments == expected_arguments_override
    else:
        actual_arguments = json.loads(actual_arguments)
        assert 'code' in actual_arguments, f"code not found in {json.dumps(actual_arguments)}"
        code = actual_arguments["code"]
        assert isinstance(code, str), f"Expected code to be a string, got {type(code)}: {json.dumps(code)}"
        assert re.match(r'''print\(("[Hh]ello,? [Ww]orld!?"|'[Hh]ello,? [Ww]orld!?')\)''', code), f'Expected hello world, got {code}'


@pytest.mark.slow
@pytest.mark.parametrize("expected_arguments_override,hf_repo,template_override", [
    (None,                 "bartowski/DeepSeek-R1-Distill-Qwen-7B-GGUF", None),
    # (None,                 "bartowski/DeepSeek-R1-Distill-Qwen-7B-GGUF", "chatml"),

    (None,                 "bartowski/Phi-3.5-mini-instruct-GGUF",      None),
    (None,                 "bartowski/Phi-3.5-mini-instruct-GGUF",      "chatml"),

    (None,                 "bartowski/functionary-small-v3.2-GGUF:Q8_0",       ("meetkai-functionary-medium-v3.2", None)),
    (None,                 "bartowski/functionary-small-v3.2-GGUF:Q8_0",       "chatml"),

    ('{"code":"print("}',  "bartowski/Meta-Llama-3.1-8B-Instruct-GGUF", None),
    (None,                 "bartowski/Meta-Llama-3.1-8B-Instruct-GGUF", "chatml"),

    (None,                 "bartowski/Llama-3.2-1B-Instruct-GGUF",      ("meta-llama-Llama-3.2-3B-Instruct", None)),
    (None,                 "bartowski/Llama-3.2-1B-Instruct-GGUF",      "chatml"),

    ('{"code":"print("}',  "bartowski/Llama-3.2-3B-Instruct-GGUF",      ("meta-llama-Llama-3.2-3B-Instruct", None)),
    (None,                 "bartowski/Llama-3.2-3B-Instruct-GGUF",      "chatml"),

    (None,                 "bartowski/Qwen2.5-7B-Instruct-GGUF",        None),
    (None,                 "bartowski/Qwen2.5-7B-Instruct-GGUF",        "chatml"),

    (None,                 "bartowski/Hermes-2-Pro-Llama-3-8B-GGUF",    ("NousResearch/Hermes-2-Pro-Llama-3-8B", "tool_use")),
    (None,                 "bartowski/Hermes-2-Pro-Llama-3-8B-GGUF",    "chatml"),

    (None,                 "bartowski/Hermes-3-Llama-3.1-8B-GGUF",      ("NousResearch-Hermes-3-Llama-3.1-8B", "tool_use")),
    (None,                 "bartowski/Hermes-3-Llama-3.1-8B-GGUF",      "chatml"),

    (None,                 "bartowski/Mistral-Nemo-Instruct-2407-GGUF", None),
    (None,                 "bartowski/Mistral-Nemo-Instruct-2407-GGUF", "chatml"),

    # Note: gemma-2-2b-it knows itself as "model", not "assistant", so we don't test the ill-suited chatml on it.
    (None,                 "bartowski/gemma-2-2b-it-GGUF",              None),
])
def test_hello_world(expected_arguments_override: str | None, hf_repo: str, template_override: str | Tuple[str, str | None] | None):
    global server
    server.n_slots = 1
    server.jinja = True
    server.n_ctx = 8192
    server.n_predict = 512 # High because of DeepSeek R1
    server.model_hf_repo = hf_repo
    server.model_hf_file = None
    if isinstance(template_override, tuple):
        (template_hf_repo, template_variant) = template_override
        server.chat_template_file = f"../../../models/templates/{template_hf_repo.replace('/', '-') + ('-' + template_variant if template_variant else '')}.jinja"
        assert os.path.exists(server.chat_template_file), f"Template file {server.chat_template_file} does not exist. Run `python scripts/get_chat_template.py {template_hf_repo} {template_variant} > {server.chat_template_file}` to download the template."
    elif isinstance(template_override, str):
        server.chat_template = template_override
    server.start(timeout_seconds=TIMEOUT_SERVER_START)

    # Note: without these greedy params, Functionary v3.2 writes `def hello_world():\n    print("Hello, World!")\nhello_world()` which is correct but a pain to test.
    do_test_hello_world(expected_arguments_override, temperature=0.0, top_k=1, top_p=1.0)

if __name__ == "__main__":
    '''
        export LLAMA_CACHE=$HOME/Library/Caches/llama.cpp
        export LLAMA_SERVER_BIN_PATH=$PWD/build/bin/llama-server
    
        ( for temp in "" "--temp 0.0" "--temp 0.5" "--temp 0.75" "--temp 1.0" ; do
            for ignore_grammar in 0 1 ; do (
                export LLAMA_IGNORE_CHAT_GRAMMAR=$ignore_grammar ;
                python examples/server/tests/unit/test_tool_call.py --n 5 --seed 124 $temp --model "Qwen 2.5 Coder 7B Q4_K_M" --hf bartowski/Qwen2.5-Coder-7B-Instruct-GGUF ;
                python examples/server/tests/unit/test_tool_call.py --n 5 --seed 124 $temp --model "Functionary Small v3.2 Q4_K_M" --hf bartowski/functionary-small-v3.2-GGUF:Q4_K_M ;
                python examples/server/tests/unit/test_tool_call.py --n 5 --seed 124 $temp --model "Mistral Nemo 2407 Q4_K_M" --hf bartowski/Mistral-Nemo-Instruct-2407-GGUF ;
            ) ; done ;
            python examples/server/tests/unit/test_tool_call.py --n 5 --seed 124 $temp --model "Qwen 2.5 Coder 7B Q4_K_M" --ollama qwen2.5-coder:7b ;
            python examples/server/tests/unit/test_tool_call.py --n 5 --seed 124 $temp --model "Mistral Nemo 2407 Q6_K" --ollama mistral-nemo:12b-instruct-2407-q6_K ;
        done ) | tee misc.jsonl
        
                python examples/server/tests/unit/test_tool_call.py --n 5 --seed 124 $temp --model "Llama 3.3 Instruct 70B Q4_K_M" --hf bartowski/Llama-3.3-70B-Instruct-GGUF:Q4_K_M ;
    '''
    # get -hf and --chat-template overrides from command line
    parser = argparse.ArgumentParser(description='Run tests for the chat server.')
    parser.add_argument('--chat-template', type=str, help='Override the chat template file path')
    parser.add_argument('--model', type=str, help='Override the model Hugging Face repository', required=True)
    parser.add_argument('--hf', type=str, help='Override the model Hugging Face repository')
    parser.add_argument('--ollama', type=str, help='Override the model Hugging Face repository')
    parser.add_argument('--n', type=int, help='Number of times to run each test', default=30)
    parser.add_argument('--temp', type=float, help='Temperature')
    parser.add_argument('--top-p', type=float, help='top_p')
    parser.add_argument('--top-k', type=int, help='top_k')
    parser.add_argument('--seed', type=int, help='Random seed')

    args = parser.parse_args()

    # chat_template = args.chat_template
    n = args.n

    n_predict = 512
    
    kwargs = {}
    if args.temp is not None:
        kwargs['temperature'] = args.temp
    if args.top_p is not None:
        kwargs['top_p'] = args.top_p
    if args.top_k is not None:
        kwargs['top_k'] = args.top_k
    if args.seed is not None:
        kwargs['seed'] = args.seed
        
    server = ServerProcess()
    if args.ollama is not None:
        implementation = "ollama"
        server.server_port = 11434
        server.server_host = "localhost"
        model_id = args.ollama
        kwargs['model'] = model_id
        subprocess.check_call(["ollama", "pull", model_id])
        # print(subprocess.check_output(["ollama", "pull", model]), flush=True, file=sys.stderr)
    else:
        implementation = "llama-server" + (" (no grammar)" if os.environ.get("LLAMA_IGNORE_CHAT_GRAMMAR", "1") == "1" else "")
        server.n_slots = 1
        server.jinja = True
        server.n_predict = 512 # High because of DeepSeek R1
        server.model_hf_repo = args.hf
        server.model_hf_file = None
        model_id = args.hf
        # server.n_predict = n_predict
        # if isinstance(template_override, tuple):
        #     (template_hf_repo, template_variant) = template_override
        #     server.chat_template_file = f"../../../models/templates/{template_hf_repo.replace('/', '-') + ('-' + template_variant if template_variant else '')}.jinja"
        #     assert os.path.exists(server.chat_template_file), f"Template file {server.chat_template_file} does not exist. Run `python scripts/get_chat_template.py {template_hf_repo} {template_variant} > {server.chat_template_file}` to download the template."
        # elif isinstance(template_override, str):
        #     server.chat_template = template_override
        import atexit
        atexit.register(server.stop)
        server.start(timeout_seconds=TIMEOUT_SERVER_START)

    tests = {
        "weather": lambda: do_test_weather(**kwargs),
        "calc result": lambda: do_test_calc_result(None, 512, **kwargs),
        "hello world": lambda: do_test_hello_world(None, **kwargs),
    }
    for test_name, test in tests.items():
        success_count = 0
        failure_count = 0
        failures = []
        success_times = []
        failure_times = []
        print(f"Running {test_name}: ", file=sys.stderr, flush=True)
        for i in range(n):
            start_time = time.time()
            def elapsed():
                return time.time() - start_time
            try:
                test()
                success_times.append(elapsed())
                success_count += 1
                print('.', end='', file=sys.stderr, flush=True)
            except Exception as e:
                print('!', end='', file=sys.stderr, flush=True)
                if failure_count == 0:
                    print(f" ({e}) ", end='', file=sys.stderr, flush=True)
                failure_count += 1
                failure_times.append(elapsed())
                failures.append(str(e))
        print('\n', file=sys.stderr, flush=True)
        print(json.dumps(dict(
            model=args.model,
            implementation=implementation,
            model_id=model_id,
            test=test_name,
            temp=args.temp,
            top_p=args.top_p,
            top_k=args.top_k,
            success_ratio=float(success_count) / n,
            avg_time=mean(success_times + failure_times),
            median_time=median(success_times + failure_times),
            success_count=success_count,
            success_times=success_times,
            failure_count=failure_count,
            failure_times=failure_times,
            failures=list(set(failures)),
        )) + '\n', flush=True)