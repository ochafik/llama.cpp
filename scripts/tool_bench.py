#!/usr/bin/env uv run
# /// script
# requires-python = ">=3.10"
# dependencies = [
#     "pytest",
#     "numpy",
#     "pandas",
#     "matplotlib",
#     "seaborn",
#     "requests",
#     "wget",
# ]
# ///
'''
    cmake --build build -j && ( \
        export LLAMA_CACHE=$HOME/Library/Caches/llama.cpp ;
        export LLAMA_SERVER_BIN_PATH=$PWD/build/bin/llama-server ;
        export ARGS=( --n=10 --temps=0,0.5,0.75,1,1.5,2,5, --append=all.jsonl ) ;
        ./scripts/tool_bench.py ${ARGS[@]} --model "Qwen 2.5 Coder 7B Q4_K_M"      --hf bartowski/Qwen2.5-Coder-7B-Instruct-GGUF  --ollama qwen2.5-coder:7b ;
        ./scripts/tool_bench.py ${ARGS[@]} --model "Qwen 2.5 1.5B Q4_K_M"          --hf bartowski/Qwen2.5-1.5B-Instruct-GGUF      --ollama qwen2.5:1.5b-instruct-q4_K_M ;
        ./scripts/tool_bench.py ${ARGS[@]} --model "Qwen 2.5 7B Q4_K_M"            --hf bartowski/Qwen2.5-7B-Instruct-GGUF ;
        ./scripts/tool_bench.py ${ARGS[@]} --model "Llama 3.2 Instruct 1B Q4_K_M"  --hf bartowski/Llama-3.2-1B-Instruct-GGUF      --ollama llama3.2:1b-instruct-q4_K_M ;
        ./scripts/tool_bench.py ${ARGS[@]} --model "Llama 3.2 Instruct 3B Q4_K_M"  --hf bartowski/Llama-3.2-3B-Instruct-GGUF      --ollama llama3.1:3b ;
        ./scripts/tool_bench.py ${ARGS[@]} --model "Llama 3.1 Instruct 8B Q4_K_M"  --hf bartowski/Meta-Llama-3.1-8B-Instruct-GGUF --ollama llama3.1:8b ;
        ./scripts/tool_bench.py ${ARGS[@]} --model "Llama 3.3 Instruct 70B Q4_K_M" --hf bartowski/Llama-3.3-70B-Instruct-GGUF ;
        ./scripts/tool_bench.py ${ARGS[@]} --model "Mistral Nemo 2407 Q4_K_M"      --hf bartowski/Mistral-Nemo-Instruct-2407-GGUF --ollama mistral-nemo:12b ;
        ./scripts/tool_bench.py ${ARGS[@]} --model "Functionary Small v3.2 Q4_K_M" --hf bartowski/functionary-small-v3.2-GGUF ;
    )

'''
import argparse
from contextlib import contextmanager
from statistics import mean, median
import pytest

# ensure grandparent path is in sys.path
from pathlib import Path
import sys

sys.path.insert(0, Path(__file__).parent.parent.as_posix())
print(sys.path)
from examples.server.tests.unit.test_tool_call import *


@contextmanager
def scoped_server(sp: ServerProcess):
    global server
    server = sp

    import atexit
    def stop():
        global server
        nonlocal sp
        if sp is not None:
            sp.stop()
            sp = None # type: ignore
            server = None # type: ignore
    atexit.register(stop)

    yield sp

    stop()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='Run tests for the chat server.')
    parser.add_argument('--model', type=str, help='Name of the model to test (implementation agnostic)', required=True)
    parser.add_argument('--hf', type=str, help='GGUF huggingface model repo id (+ optional quant) to test w/ llama-server')
    parser.add_argument('--hfd', type=str, help='GGUF huggingface draft model repo id (+ optional quant) to test w/ llama-server')
    parser.add_argument('--chat-template', type=str, help='Chat template override for llama-server')
    parser.add_argument('--ollama', type=str, help='Ollama model tag to test')
    parser.add_argument('--n', type=int, help='Number of times to run each test', default=30)
    parser.add_argument('--temps', type=str, help='Comma-separated list of temperatures')
    parser.add_argument('--top-p', type=float, help='top_p')
    parser.add_argument('--top-k', type=int, help='top_k')
    parser.add_argument('--seed', type=int, help='Random seed')
    parser.add_argument('--port', type=int, help='llama-server port')
    parser.add_argument('--output', type=str, help='Output JSON file')
    parser.add_argument('--append', type=str, help='Output JSON file')


    args = parser.parse_args()

    # Check only one of output and append
    assert (args.output is None) != (args.append is None), "Exactly one of --output and --append must be specified"

    # chat_template = args.chat_template
    n = args.n

    n_predict = 512

    with open(args.output or args.append, 'w' if args.output else 'a') as output_file:

        def run(*, implementation: str, model_id: str, temp: float | None = None, output_kwargs={}, request_kwargs={}):
            request_kwargs = {**request_kwargs}
            if temp is not None:
                request_kwargs['temperature'] = temp
            if args.top_p is not None:
                request_kwargs['top_p'] = args.top_p
            if args.top_k is not None:
                request_kwargs['top_k'] = args.top_k
            if args.seed is not None:
                request_kwargs['seed'] = args.seed

            request_kwargs['cache_prompt'] = False

            tests = {
                "hello world": lambda: do_test_hello_world(**request_kwargs),
                "weather": lambda: do_test_weather(**request_kwargs),
                "calc result": lambda: do_test_calc_result(None, 512, **request_kwargs),
            }
            for test_name, test in tests.items():
                success_count = 0
                failure_count = 0
                failures = []
                success_times = []
                failure_times = []
                print(f"Running {test_name} ({implementation}, {args.model}): ", file=sys.stderr, flush=True)
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
                output_file.write(json.dumps({**output_kwargs, **dict(
                    model=args.model,
                    implementation=implementation,
                    model_id=model_id,
                    test=test_name,
                    temp=temp,
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
                )}) + '\n')
                output_file.flush()

        temps = [float(temp) if temp != "" else None for temp in args.temps.split(',')] if args.temps is not None else [None]
        for temp in temps:
            if args.hf is not None:
                server = ServerProcess()
                server.n_slots = 1
                server.jinja = True
                server.n_predict = 512 # High because of DeepSeek R1
                server.model_hf_repo = args.hf
                server.model_hf_file = None
                server.model_draft_hf_repo = args.hfd
                server.chat_template = args.chat_template
                if args.port is not None:
                    server.server_port = args.port
                # server.debug = True

                with scoped_server(server):
                    server.start(timeout_seconds=TIMEOUT_SERVER_START)
                    for ignore_chat_grammar in [False, True]:
                        run(
                            implementation="llama-server" + (" (no grammar)" if ignore_chat_grammar else ""),
                            model_id=args.hf,
                            temp=temp,
                            output_kwargs=dict(
                                chat_template=args.chat_template,
                            ),
                            request_kwargs=dict(
                                ignore_chat_grammar=ignore_chat_grammar,
                            ),
                        )

            if args.ollama is not None:
                server = ServerProcess()
                server.server_port = 11434
                server.server_host = "localhost"
                subprocess.check_call(["ollama", "pull", args.ollama])

                with scoped_server(server):
                    run(
                        implementation="ollama",
                        model_id=args.ollama,
                        temp=temp,
                        output_kwargs=dict(
                            chat_template=None,
                        ),
                        request_kwargs=dict(
                            model=args.ollama,
                        ),
                    )
