from enum import Enum
from pathlib import Path
import sys
from time import sleep
import typer
from pydantic import BaseModel, Json, TypeAdapter
from typing import Annotated, Any, Callable, Dict, List, Union, Optional, Type
import json

from openai import OpenAI
from examples.agent.openapi_client import OpenAPIMethod, openapi_methods_from_endpoint
from examples.agent.tools.std_tools import StandardTools
from examples.agent.utils import collect_functions, load_module
from examples.openai.subprocesses import spawn_subprocess

class APIType(str, Enum):
    OpenAI = 'openai'
    LlamaCpp = 'llama.cpp'

def main(
    goal: Annotated[str, typer.Option()],
    tools: Optional[List[str]] = None,
    format: Annotated[Optional[str], typer.Option(help="The output format: either a Python type (e.g. 'float' or a Pydantic model defined in one of the tool files), or a JSON schema, e.g. '{\"format\": \"date\"}'")] = None,
    max_iterations: Optional[int] = 10,
    std_tools: Optional[bool] = False,
    api_key: Optional[str] = None,
    verbose: bool = False,
    allow_response_schema: Optional[bool] = None,

    model = "gpt-4o",
    api_type: APIType = APIType.LlamaCpp,
    endpoint: str = 'http://localhost:8080',
    # endpoint: Optional[str] = None,
    # model: Optional[Annotated[str, typer.Option("--model", "-m")]] = None,# = "models/7B/ggml-model-f16.gguf",
    # model_url: Optional[Annotated[str, typer.Option("--model-url", "-mu")]] = None,
    # hf_repo: Optional[Annotated[str, typer.Option("--hf-repo", "-hfr")]] = None,
    # hf_file: Optional[Annotated[str, typer.Option("--hf-file", "-hff")]] = None,
    # endpoint: str = 'http://localhost:8080/v1/chat/completions',
    # greedy: Optional[bool] = True,
    temperature: Optional[float] = 0,
):
    functions = []
    types: Dict[str, type] = {}
    for f in (tools or []):
        if f.startswith('http://') or f.startswith('https://'):
            functions.extend(openapi_methods_from_endpoint(f))
        else:
            module = load_module(f)
            functions.extend(collect_functions(module))
            types.update({
                k: v
                for k, v in module.__dict__.items()
                if isinstance(v, type)
            })

    if std_tools:
        functions.extend(collect_functions(StandardTools))

    sys.stdout.write(f'🛠️  {", ".join(fn.__name__ for fn in functions)}\n')

    client = OpenAI(
        base_url=endpoint,
        api_key=api_key,
    )

    tool_map = {fn.__name__: fn for fn in functions}
    tools = [
        dict(
            type="function",
            function=dict(
                name=fn.__name__,
                description=fn.__doc__ or '',
                parameters=TypeAdapter(fn).json_schema(),
            )
        )
        for fn in functions
    ]
    response_tool_choice = None
    response_type_adapter = None
    response_format = None
    tool_choice = None

    response_model: Union[type, Json[Any]] = None #str
    if format:
        if format in types:
            response_model = types[format]
        elif format == 'json':
            response_model = {}
        else:
            try:
                response_model = json.loads(format)
            except:
                response_model = eval(format)

    if response_model:
        i = 0
        while (response_tool_choice := f'output{i if i > 0 else ""}') in tool_map:
            i += 1
        response_tool_choice = response_tool_choice
        tool_choice = 'required'

        if isinstance(response_model, dict):
            schema = response_model
        else:
            response_type_adapter = TypeAdapter(response_model)
            schema = response_type_adapter.json_schema()
        
        tools.append(dict(
            type="function",
            function=dict(
                name=response_tool_choice,
                description="Response type",
                parameters=schema,
            )
        ))
        response_format = {"type": "json_object"}
        if allow_response_schema:
            response_format['schema'] = schema

    messages=[dict(role="user", content=goal)]

    result_content = None   

    while result_content is None:
        if verbose:
            sys.stderr.write(f'\n# REQUEST:\n\nclient.chat.completions.create{json.dumps(dict(model=model, messages=messages, tools=tools, tool_choice=tool_choice, response_format=response_format), indent=2)}\n\n')
        choice = client.chat.completions.create(
            model=model,
            messages=messages,
            tools=tools,
            tool_choice=tool_choice,
            response_format=response_format,
            temperature=temperature,
        ).choices[0]

        if choice.finish_reason == "tool_calls":
            messages.append(choice.message)
            for tool_call in choice.message.tool_calls:
                if choice.message.content:
                    print(f'💭 {choice.message.content}')

                if tool_call.function.name == response_tool_choice:
                    result_content = tool_call.function.arguments
                    break

                args = json.loads(tool_call.function.arguments)

                pretty_call = f'{tool_call.function.name}({", ".join(f"{k}={v.model_dump_json() if isinstance(v, BaseModel) else json.dumps(v)}" for k, v in args.items())})'
                sys.stdout.write(f'⚙️  {pretty_call}')
                sys.stdout.flush()
                tool_result = tool_map[tool_call.function.name](**args)
                sys.stdout.write(f" → {tool_result}\n")
                messages.append(dict(
                    tool_call_id=tool_call.id,
                    role="tool",
                    name=tool_call.function.name,
                    content=f'{tool_result}',
                ))
        else:
            result_content = choice.message.content
            break

    result = response_type_adapter.validate_json(result_content) if response_type_adapter else result_content
    print(result if response_model else f'➡️ {result}')

if __name__ == '__main__':
    typer.run(main)

