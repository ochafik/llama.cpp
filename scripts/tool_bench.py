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
#     "typer",
# ]
# ///
'''
    cmake --build build -j && ( \
        export RETRIES=3 ;
        export LLAMA_CACHE=$HOME/Library/Caches/llama.cpp ;
        export LLAMA_SERVER_BIN_PATH=$PWD/build/bin/llama-server ;
        export ARGS=( --n 10 --temp -1 --temp 0 --temp 0.5 --temp 0.75 --temp 1 --temp 1.5 --temp 2 --temp 5 ) ;
        ./scripts/tool_bench.py run ${ARGS[@]} --model "Qwen 2.5 1.5B Q4_K_M"                  --output qwen1.5b.jsonl  --hf bartowski/Qwen2.5-1.5B-Instruct-GGUF         --ollama qwen2.5:1.5b-instruct-q4_K_M ;
        ./scripts/tool_bench.py run ${ARGS[@]} --model "Llama 3.2 Instruct 1B Q4_K_M"          --output llama1b.jsonl   --hf bartowski/Llama-3.2-1B-Instruct-GGUF         --ollama llama3.2:1b-instruct-q4_K_M ;
        ./scripts/tool_bench.py run ${ARGS[@]} --model "Llama 3.2 Instruct 3B Q4_K_M"          --output llama3b.jsonl   --hf bartowski/Llama-3.2-3B-Instruct-GGUF         --ollama llama3.1:3b ;
        ./scripts/tool_bench.py run ${ARGS[@]} --model "Llama 3.1 Instruct 8B Q4_K_M"          --output llama8b.jsonl   --hf bartowski/Meta-Llama-3.1-8B-Instruct-GGUF    --ollama llama3.1:8b ;
        ./scripts/tool_bench.py run ${ARGS[@]} --model "Qwen 2.5 Coder 7B Q4_K_M"              --output qwenc7b.jsonl   --hf bartowski/Qwen2.5-Coder-7B-Instruct-GGUF     --ollama qwen2.5-coder:7b ;
        ./scripts/tool_bench.py run ${ARGS[@]} --model "Qwen 2.5 7B Q4_K_M"                    --output qwen7b.jsonl    --hf bartowski/Qwen2.5-7B-Instruct-GGUF ;
        ./scripts/tool_bench.py run ${ARGS[@]} --model "Llama 3.3 Instruct 70B Q4_K_M"         --output llama70b.jsonl  --hf bartowski/Llama-3.3-70B-Instruct-GGUF ;
        ./scripts/tool_bench.py run ${ARGS[@]} --model "Mistral Nemo 2407 Q4_K_M"              --output nemo.jsonl      --hf bartowski/Mistral-Nemo-Instruct-2407-GGUF    --ollama mistral-nemo:12b ;
        ./scripts/tool_bench.py run ${ARGS[@]} --model "Functionary Small v3.2 Q4_K_M"         --output funcsmall.jsonl --hf bartowski/functionary-small-v3.2-GGUF ;
        ./scripts/tool_bench.py run ${ARGS[@]} --model "DeepSeek R1 Distill Qwen 1.5B Q4_K_M"  --output dsqw1.5b.jsonl  --hf bartowski/DeepSeek-R1-Distill-Qwen-1.5B-GGUF --ollama deepseek-r1:1.5b ;
    )
    
    ./scripts/tool_bench.py plot ../qw.jsonl --output ../qw.png

'''

from contextlib import contextmanager
from pathlib import Path
from pathlib import Path
from statistics import mean, median
from typing import Annotated, List, Optional
from typing import Dict, List, Tuple, Set, Any
import json
import logging
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import seaborn as sns
import subprocess
import sys
import time

sys.path.insert(0, Path(__file__).parent.parent.as_posix())
print(sys.path)
from examples.server.tests.utils import ServerProcess
from examples.server.tests.unit.test_tool_call import TIMEOUT_SERVER_START, do_test_calc_result, do_test_hello_world, do_test_weather


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


import typer

app = typer.Typer()


logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

@app.command()
def plot(files: List[Path], output: Optional[Path] = None):
    
    lines: List[Dict] = []
    for file in files:
        if not file.exists():
            logger.error(f"File not found: {file}")
            continue
            
        try:
            with file.open() as f:
                raw_data = f.read()
            logger.info(f"Reading {file} ({len(raw_data)} bytes)")
            
            for line_num, line in enumerate(raw_data.split('\n'), 1):
                line = line.strip()
                if not line:
                    continue
                try:
                    record = json.loads(line)
                    lines.append(record)
                except json.JSONDecodeError as e:
                    logger.warning(f"Invalid JSON at {file}:{line_num} - {e}")
        except Exception as e:
            logger.error(f"Error processing {file}: {e}")

    if not lines:
        raise Exception("No valid data was loaded")
    
    data_dict: Dict[Tuple, float] = {}
    models: List[str] = []
    temps = set()
    tests = set()
    impls = set()
    for rec in lines:
        try:
            model = rec["model"]
            temp = rec["temp"]
            impl = rec["implementation"]
            test = rec["test"]
            success = rec["success_ratio"]
            
            
            data_dict[(model, temp, impl, test)] = success
            
            if model not in models:
                models.append(model)
            temps.add(temp)
            tests.add(test)
            impls.add(impl)
            
        except KeyError as e:
            logger.warning(f"Missing required field in record: {e}")

    # Sort the collected values
    temps = list(sorted(temps, key=lambda x: x if x is not None else -1))
    tests = list(sorted(tests))
    impls = list(sorted(impls))

    
    logger.info(f"Processed {len(lines)} lines")
    logger.info(f"Found {len(data_dict)} valid data points")
    logger.info(f"Models: {models}")
    logger.info(f"Temperatures: {temps}")
    logger.info(f"Tests: {tests}")
    logger.info(f"Implementations: {impls}")

    
    matrix = []
    index = []
    
    all_cols = [
        (impl, test)
        for impl in impls
        for test in tests
    ]
    for model in models:
        for temp in temps:
            index.append(f"{model} @ {temp}")
            row_vals = [
                data_dict.get((model, temp, impl, test), np.nan)
                for impl, test in all_cols
            ]
            matrix.append(row_vals)
    
    columns = [f"{impl}\n({test})" for impl, test in all_cols]

    df = pd.DataFrame(matrix, index=index, columns=columns)

    plt.figure(figsize=(12, 6))
            
    sns.heatmap(
        df, annot=True, cmap="RdYlGn", vmin=0.0, vmax=1.0, cbar=True, fmt=".2f", center=0.5, square=True, linewidths=0.5,
        cbar_kws={"label": "Success Ratio"},
    )
    
    plt.title("Tool Call Bench\nSuccess Ratios by Implementation & Test", pad=20)
    plt.xlabel("Implementation and Test", labelpad=10)
    plt.ylabel("Model @ Temperature", labelpad=10)
    
    plt.xticks(rotation=45, ha='right')
    plt.yticks(rotation=0)
    
    plt.tight_layout()
    
    if output:
        plt.savefig(output, dpi=300, bbox_inches='tight')
        logger.info(f"Plot saved to {output}")
    else:
        plt.show()
    
@app.command()
def run(
    output: Annotated[Path, typer.Option(help="Output JSON file")],
    model: Annotated[Optional[str], typer.Option(help="Name of the model to test (implementation agnostic)")] = None,
    hf: Annotated[Optional[str], typer.Option(help="GGUF huggingface model repo id (+ optional quant) to test w/ llama-server")] = None,
    chat_template: Annotated[Optional[str], typer.Option(help="Chat template override for llama-server")] = None,
    ollama: Annotated[Optional[str], typer.Option(help="Ollama model tag to test")] = None,
    n: Annotated[int, typer.Option(help="Number of times to run each test")] = 10,
    temp: Annotated[Optional[List[float]], typer.Option(help="Set of temperatures to test")] = None,
    top_p: Annotated[Optional[float], typer.Option(help="top_p")] = None,
    top_k: Annotated[Optional[int], typer.Option(help="top_k")] = None,
    seed: Annotated[Optional[int], typer.Option(help="Random seed")] = None,
    port: Annotated[int, typer.Option(help="llama-server port")] = 8084,
    force: Annotated[bool, typer.Option(help="Force overwrite of output file")] = False,
    append: Annotated[bool, typer.Option(help="Append to output file")] = False,
):
    # Check only one of output and append
    
    n_predict = 512

    assert force or not output.exists(), f"Output file already exists: {output}; use --force to overwrite"
        
    with output.open('a' if append else 'w') as output_file:

        def run(server: ServerProcess, *, implementation: str, model_id: str, temp: float | None = None, output_kwargs={}, request_kwargs={}):
            request_kwargs = {**request_kwargs}
            if temp is not None:
                request_kwargs['temperature'] = temp
            if top_p is not None:
                request_kwargs['top_p'] = top_p
            if top_k is not None:
                request_kwargs['top_k'] = top_k
            if seed is not None:
                request_kwargs['seed'] = seed

            request_kwargs['cache_prompt'] = False

            tests = {
                "hello world": lambda server: do_test_hello_world(server, **request_kwargs),
                "weather": lambda server: do_test_weather(server, **request_kwargs),
                "calc result": lambda server: do_test_calc_result(server, None, 512, **request_kwargs),
            }
            for test_name, test in tests.items():
                success_count = 0
                failure_count = 0
                failures = []
                success_times = []
                failure_times = []
                print(f"Running {test_name} ({implementation}, {model}): ", file=sys.stderr, flush=True)
                for i in range(n):
                    start_time = time.time()
                    def elapsed():
                        return time.time() - start_time
                    try:
                        test(server)
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
                    model=model,
                    implementation=implementation,
                    model_id=model_id,
                    test=test_name,
                    temp=t,
                    top_p=top_p,
                    top_k=top_k,
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

        for t in [None] if temp is None else [t if t >= 0 else None for t in temp]:
            if hf is not None:
                server = ServerProcess()
                server.n_slots = 1
                server.jinja = True
                server.n_predict = 512 # High because of DeepSeek R1
                server.model_hf_repo = hf
                server.model_hf_file = None
                server.chat_template = chat_template
                if port is not None:
                    server.server_port = port
                # server.debug = True

                with scoped_server(server):
                    server.start(timeout_seconds=TIMEOUT_SERVER_START)
                    for ignore_chat_grammar in [False]:
                        run(
                            server,
                            implementation="llama-server" + (" (no grammar)" if ignore_chat_grammar else ""),
                            model_id=hf,
                            temp=t,
                            output_kwargs=dict(
                                chat_template=chat_template,
                            ),
                            request_kwargs=dict(
                                ignore_chat_grammar=ignore_chat_grammar,
                            ),
                        )

            if ollama is not None:
                server = ServerProcess()
                server.server_port = 11434
                server.server_host = "localhost"
                subprocess.check_call(["ollama", "pull", ollama])

                with scoped_server(server):
                    run(
                        server,
                        implementation="ollama",
                        model_id=ollama,
                        temp=t,
                        output_kwargs=dict(
                            chat_template=None,
                        ),
                        request_kwargs=dict(
                            model=ollama,
                        ),
                    )

if __name__ == "__main__":
    app()