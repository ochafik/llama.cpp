# examples.openai: OpenAI-compatibility layer on top of server.cpp

New Python OpenAI API compatibility server, which calls into / spawns the C++ server under the hood:

```bash
python -m examples.openai.server --model model.gguf
```

## Prerequisites

Note: To get conda, just install Miniforge (it's OSS): https://github.com/conda-forge/miniforge

```bash
conda create -n agent python=3.11
conda activate agent
pip install -r examples/openai/requirements.txt
```

## Features

The new [examples/openai/server.py](./server.py):

- Supports grammar-constrained tool calling for **all** models (incl. Mixtral 7x8B)

    - Optimised support for Functionary & Nous Hermes, easy to extend to other tool-calling schemes

    - Generic support w/ JSON schema that guides the model towards tool usage (at the cost of extra tokens):

        ```ts
          {
            // original_thought: string,
            thought_about_next_step_only: string,
            next_step: {tool_calls: {name: string, arguments: any}} | {result: T}
          }
          // Where T is the output JSON schema, or 'any'
        ```

        - Option to publicise schemas to models as TypeScript signatures (as for Functionary) or JSON schema.

        - Supports models that require user/assistant alternance (like Mixtral Instruct) by merging system messages into user messages.

- Spawns the C++ [llama.cpp server](../server) under the hood (unless passed `--endpoint`), but only uses its non-chat endpoint

  (depending on the prompting strategy, we weave the tool & output schema along with the chat template into the raw model grammar constraints)

- Uses the actual Jinja2 templates stored in the GGUF models

- Will eventually also spawn `whisper.cpp` and another server subprocess for the embeddings endpoint

Rationale: the C++ server lacks some OpenAI compatibility features (and can't realistically keep up with prompt templates w/o bringing in too many dependencies), this new layer could allow focusing the C++ server on serving efficiency and delegate OAI compliance to a layer easier to maintain.

## Test

If you want to see tools in action, look at the [agent example](../agent). Otherwise:

Start the server in Terminal 1:

```bash
python -m examples.openai --model  ~/AI/Models/mixtral-8x7b-instruct-v0.1.Q4_K_M.gguf
```

Query it in Terminal 2 (or use it from any framework that makes use of tools: note tool calls are guaranteed to comply to the schema, so retries are likely not necessary!):

```bash
curl http://localhost:8080/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model": "gpt-3.5-turbo",
    "tools": [{
          "type": "function",
          "function": {
              "name": "get_current_weather",
              "description": "Get the current weather",
              "parameters": {
                  "type": "object",
                  "properties": {
                      "location": {
                          "type": "string",
                          "description": "The city and state, e.g. San Francisco, CA"
                      },
                      "format": {
                          "type": "string",
                          "enum": ["celsius", "fahrenheit"],
                          "description": "The temperature unit to use. Infer this from the users location."
                      }
                  },
                  "required": ["location", "format"]
              }
          }
      }, {
          "type": "function",
          "function": {
              "name": "get_n_day_weather_forecast",
              "description": "Get an N-day weather forecast",
              "parameters": {
                  "type": "object",
                  "properties": {
                      "location": {
                          "type": "string",
                          "description": "The city and state, e.g. San Francisco, CA"
                      },
                      "format": {
                          "type": "string",
                          "enum": ["celsius", "fahrenheit"],
                          "description": "The temperature unit to use. Infer this from the users location."
                      },
                      "num_days": {
                          "type": "integer",
                          "description": "The number of days to forecast"
                      }
                  },
                  "required": ["location", "format", "num_days"]
              }
          }
      }],
    "messages": [
      {"role": "system", "content": "Do not make assumptions about what values to plug into functions. Ask for clarification if a user request is ambiguous."},
      {"role": "user", "content": "what is the weather going to be like in San Francisco and Glasgow over the next 4 days"}
    ]
  }'
```

<details>
<summary>Show output</summary>

```json
{
  "id": "chatcmpl-3095057176",
  "object": "chat.completion",
  "created": 1711726921,
  "model": "gpt-3.5-turbo",
  "choices": [
    {
      "index": 0,
      "message": {
        "role": "assistant",
        "name": null,
        "tool_call_id": null,
        "content": "In order to provide the required information, I need to call the get_n_day_weather_forecast function twice, once for San Francisco and once for Glasgow.",
        "tool_calls": [
          {
            "id": "call_970977",
            "type": "function",
            "function": {
              "name": "get_n_day_weather_forecast",
              "arguments": {
                "location": "San Francisco, CA",
                "format": "celsius",
                "num_days": 4
              }
            }
          }
        ]
      },
      "logprobs": null,
      "finish_reason": "tool_calls"
    }
  ],
  "usage": {
    "prompt_tokens": 546,
    "completion_tokens": 118,
    "total_tokens": 664
  },
  "system_fingerprint": "...",
  "error": null
}
```

</details>

## TODO

- Add https://github.com/jinja2cpp/Jinja2Cpp dep to C++ to move logic to server.cpp?

- Embedding endpoint w/ distinct server subprocess

- Evaluate options for session caching

    - Pass session id & store / read from file?

    - Support parent session ids for trees of thought?

    - Support precaching long prompts from CLI / read session files?

- Follow-ups

    - Remove OAI support from server

    - Remove non-Python json-schema-to-grammar versions

    - Reach out to frameworks to advertise new option.

## Function call

```bash
curl -X POST http://localhost:8080/v1/chat/completions -d '{
  "messages": [
    {"role": "user", "content": "What is 15315*3/55 ?"}
  ],
  "tools": [
    {
      "type": "function",
      "function": {
        "name": "eval_python",
        "parameters": {
          "properties": {
            "expression": {"type": "string"}
          }
        }
      }
    }
  ],
  "max_tokens": 100
}' -N

curl -X POST http://localhost:8080/v1/chat/completions -d '{
  "messages": [
    {"role": "user", "content": "What is life?"}
  ],
  "tools": [
    {
      "type": "function",
      "function": {
        "name": "eval_python",
        "parameters": {
          "properties": {
            "expression": {"type": "string"}
          }
        }
      }
    }
  ],
  "max_tokens": 100
}' -N
# "stream": true

```

Depending on the model, different templates need to be passed.

Grammar support was extended to support "triggering" grammar compliance after detection of any trigger word. They're similar to stop words, except they just enable the grammar (which needs to be able to consume them).

A lightweight Python proxy does the heavy lifting of prepending the right system prompt and provide the right grammar + trigger words if needed, and parsing the output. And actually, it also uses the real chat template and only calls the /v1/completions endpoint (not the /v1/chat/completions).

TODO:
- Attempt to import official libraries for tool calls?
  - https://github.com/meta-llama/llama-agentic-system for Llama 3.1
  - https://github.com/NousResearch/Hermes-Function-Calling for Hermes Pro 2 models

### Llama 3.1 Function Calling

Note: 8B model officially doesn't work well with function calling. Prefer the 70B or 405B models.

https://huggingface.co/blog/llama31#built-in-tool-calling
https://docs.together.ai/docs/llama-3-function-calling
https://github.com/meta-llama/llama-agentic-system

To enable the builtin `ipython`, `brave_search` and `wolfram_alpha` tools, you need to pass the following tools:

*   Builtin Tools (must pass each of them in `tools` param to enable it, and signature must match):

    ```json
    [
      {
        "type": "function",
        "function": {
          "name": "ipython",
          "description": "Runs code in an ipython interpreter and returns the result of the execution after 60 seconds.",
          "parameters": {
            "type": "object",
            "properties": {"code": {"type": "string"}},
            "required": ["code"]
          }
        }
      },
      {
        "type": "function",
        "function": {
          "name": "brave_search",
          "description": "Executes a web search with Brave.",
          "parameters": {
            "type": "object",
            "properties": {"code": {"type": "query"}},
            "required": ["query"]
          }
        }
      },
      {
        "type": "function",
        "function": {
          "name": "wolfram_alpha",
          "description": "Executes a query with Wolfram Alpha.",
          "parameters": {
            "type": "object",
            "properties": {"code": {"type": "query"}},
            "required": ["query"]
          }
        }
      },
    ]
    ```

*   System prompt:

    ```jinja2
    {% set has_ipython = false %}
    {% set predefined_tools = ['brave_search', 'wolfram_alpha'] %}
    {% set displayed_tools = [] %}
    {% set other_tools = [] %}

    {% for tool in tools %}
        {% if tool.function.name == 'ipython' %}
            {% set has_ipython = true %}
        {% else if tool.function.name in predefined_tools %}
            {% set _ = displayed_tools.append(tool.function.name) %}
        {% else %}
            {% set _ = other_tools.append(tool) %}
        {% endif %}
    {% endfor %}

    {% if has_ipython %}
    Environment: ipython
    {% endif %}
    {% if displayed_tools %}
    Tools: {{ displayed_tools | join(', ') }}
    {% endif %}

    Cutting Knowledge Date: {{ cutting_knowledge_date }}
    Today's Date: {{ todays_date }}

    You are a helpful assistant with tool calling capabilities. When you receive a tool call response, use the output to format an answer to the orginal user question.

    {% if other_tools %}
    You have access to the following functions:

    {{ other_tools | tojson(indent=2) }}
    
    If you choose to call a function ONLY reply in the following format with no prefix or suffix:

    <function=example_function_name>{{\"example_name\": \"example_value\"}}</function>

    Reminder:
    - Function calls MUST follow the specified format, start with <function= and end with </function>
    - Required parameters MUST be specified
    - Only call one function at a time
    - Put the entire function call reply on one line
    - If there is no function call available, answer the question like normal with your current knowledge and do not tell the user about function calls
    {% endif %}
    ```

*   Antiprompts:

    ```json
    "stop": [
      "<eom_id>"
    ],
    "grammar_trigger_words": [
      "<function=",
      "<|python_tag|>"
    ]
    ```

*   Grammar: `

    ```
    root ::=
        ("<function=" ("foo" ">" foo-args | "bar" ">" bar-args ... ) "</function>")*
        "<|python_tag|>" .*
    foo-args ::= ...normal conversion of JSON schema for args object...
    bar-args ::= ...normal conversion of JSON schema for args object...
    ```

### Functionary v2 Function Calling

https://github.com/MeetKai/functionary

### Functionary v3 Llama 3 Function Calling

https://github.com/MeetKai/functionary/blob/main/tests/prompt_test_v3.llama3.txt

*   System prompt

    ```
    You are capable of executing available function(s) if required.
    Only execute function(s) when absolutely necessary.
    Ask for the required input to:recipient==all
    Use JSON for function arguments.
    Respond in this format:
    >>>${recipient}
    ${content}
    Available functions:
    // Supported function definitions that should be called when necessary.
    namespace functions {

    // Get the current weather
    type get_current_weather = (_: {
    // The city and state, e.g. San Francisco, CA
    location: string,
    }) => any;

    } // namespace functions<|eot_id|>
    ```

*   Antiprompts:

    ```json
    "strip_prefix": ">>>all\n",
    "grammar_trigger_words": [
      ">>>function1\n",
      ">>>function2\n",
      ">>>function3\n",
      ...
    ]
    ```

*   Grammar: `

    ```
    root ::= (">>>function1\n" function1-args | ">>>function2\n" function2-args ...)*
    ...
    ```

### Functionary v3.2 Llama 3 Function Calling

https://github.com/MeetKai/functionary
https://huggingface.co/meetkai/functionary-small-v3.2
https://github.com/MeetKai/functionary/blob/main/tests/prompt_test_v3.llama3.txt

### Hermes 3 Llama 3.1 Function Calling

https://huggingface.co/NousResearch/Hermes-3-Llama-3.1-8B#prompt-format-for-function-calling
https://github.com/NousResearch/Hermes-Function-Calling

*   System prompt

    ```
    You are a function calling AI model. You are provided with function signatures within <tools></tools> XML tags.
    
    You may call one or more functions to assist with the user query. Don't make assumptions about what values to plug into functions.
    
    Here are the available tools: <tools>{{ tools | tojson(indent=2) }}</tools>
    
    Use the following pydantic model json schema for each tool call you will make:
    
    {"properties": {"arguments": {"title": "Arguments", "type": "object"}, "name": {"title": "Name", "type": "string"}}, "required": ["arguments", "name"], "title": "FunctionCall", "type": "object"}
    
    For each function call return a json object with function name and arguments within <tool_call></tool_call> XML tags as follows:

    <tool_call>
    {"arguments": <args-dict>, "name": <function-name>}
    </tool_call>
    ```

*   For just JSON schema output:

    ```
    You are a helpful assistant that answers in JSON. Here's the json schema you must adhere to:\n<schema>\n{{ jsonSchema | tojson(indent=2) }}\n</schema>
    ```

### Ad-hoc Function Calling (any model)

Most models work reasonably well with this (including Mixtral 8x7B).

*   System prompt (uses TS signatures to save tokens):

    ```
    You are a function calling AI model.
    Here are the tools available:
    {typeScriptToolSignatures}

    Please respond in JSON format with the following schema:

    {
      thought_about_next_step_only: string,
      next_step: {
        tool_calls: {
          name: string,
          arguments: any
        }[]
      } | {
        result: number
      }
    }
    ```

*   Grammar: from JSON schema matching the TS schema
