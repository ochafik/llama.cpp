# llama-cpp swiss-army-knife CLI

This example federates just about every other example into a command line.

## Building

Build from scratch with `-DLLAMA_CLI=1`:

```bash
rm -fR build

cmake -B build \
    -DLLAMA_CLI=1 \
    -DLLAMA_METAL_EMBED_LIBRARY=1 \
    -DLLAMA_LTO=1

cmake --build build -t llama-cpp -j
cmake --install build
```

## Usage

Simple chat:

```bash
llama-cpp run -clm -hfr microsoft/Phi-3-mini-4k-instruct-gguf -hff Phi-3-mini-4k-instruct-q4.gguf
```

Simple server on http://localhost:8080 (serves a web page + provides OpenAI-compatible endpoint):

```bash
llama-cpp serve -hfr microsoft/Phi-3-mini-4k-instruct-gguf -hff Phi-3-mini-4k-instruct-q4.gguf
```

```bash
llama-cpp
...
```

## TODOs

- Create a `homebrew` recipe
- Package as a `Snap` (maybe distinct packages for various backends)
- Command `chat` as an alias for `run -clm`?
