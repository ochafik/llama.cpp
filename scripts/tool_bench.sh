#!/bin/bash
set -euo pipefail

cmake --build build -j

export RETRIES=3
export CONSTRAIN_PYTHON_TOOL_CODE=0
export LLAMA_MASK=1
export LLAMA_CACHE=$HOME/Library/Caches/llama.cpp
export LLAMA_SERVER_BIN_PATH=$PWD/build/bin/llama-server
export ARGS=(
    --llama-baseline=/opt/homebrew/bin/llama-server
    --n 30
    --temp -1
    --temp 0
    --temp 0.5
    --temp 0.75
    --temp 1
    --temp 1.5
    --temp 2
    --temp 5
)

./scripts/tool_bench.py run ${ARGS[@]} --model "Llama 3.2 Instruct 1B Q4_K_M"          --output llama1b.jsonl   --hf bartowski/Llama-3.2-1B-Instruct-GGUF         --ollama llama3.2:1b-instruct-q4_K_M ;
./scripts/tool_bench.py run ${ARGS[@]} --model "Llama 3.2 Instruct 3B Q4_K_M"          --output llama3b.jsonl   --hf bartowski/Llama-3.2-3B-Instruct-GGUF         --ollama llama3.1:3b ;
./scripts/tool_bench.py run ${ARGS[@]} --model "Llama 3.1 Instruct 8B Q4_K_M"          --output llama8b.jsonl   --hf bartowski/Meta-Llama-3.1-8B-Instruct-GGUF    --ollama llama3.1:8b ;
./scripts/tool_bench.py run ${ARGS[@]} --model "Qwen 2.5 Coder 7B Q4_K_M"              --output qwenc7b.jsonl   --hf bartowski/Qwen2.5-Coder-7B-Instruct-GGUF     --ollama qwen2.5-coder:7b ; 
./scripts/tool_bench.py run ${ARGS[@]} --model "Qwen 2.5 1.5B Q4_K_M"                  --output qwen1.5b.jsonl  --hf bartowski/Qwen2.5-1.5B-Instruct-GGUF         --ollama qwen2.5:1.5b-instruct-q4_K_M ;
