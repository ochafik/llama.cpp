# llama-download

A command-line tool for downloading and managing GGUF models for llama.cpp.

## Build Requirements

This tool requires libcurl support. Build with:

```bash
cmake -B build -DLLAMA_CURL=ON
cmake --build build --target llama-download
```

The tool is only built when `-DLLAMA_CURL=ON` is set (enabled by default on most platforms).

## Features

- Download models from HuggingFace, Docker Hub (OCI registries), or direct URLs
- Queue multiple downloads with persistent state
- Resume interrupted downloads (if server supports Range requests)
- Check for model updates via ETag validation
- Disk space monitoring and validation
- Wait-for-network mode for unreliable connections

## Usage

```bash
# Download from HuggingFace
llama-download unsloth/phi-4-GGUF:q4_k_m
llama-download -hf bartowski/Llama-3.2-1B-Instruct-GGUF

# Download from Docker Hub / OCI registry
llama-download -dr smollm2:135M-Q4_0

# Multiple models (comma-separated)
llama-download -hf user/repo1:q4,user/repo2:q6

# Batch download from file
llama-download -f models.txt

# Preview what would be downloaded
llama-download --dry-run -hf user/model

# List cached models and queue status
llama-download --list

# Resume pending downloads
llama-download --resume

# Check for updates to cached models
llama-download --update

# Wait indefinitely for network on failure
llama-download -hf user/model --wait-for-network
```

## Options

| Option | Description |
|--------|-------------|
| `--list`, `-l` | List cached models and download queue status |
| `--resume`, `-r` | Resume pending downloads from the queue |
| `--update`, `-u` | Check cached models for updates |
| `--dry-run`, `-n` | Show what would be downloaded without downloading |
| `--input-file`, `-f` | File containing model sources (one per line) |
| `--parallel`, `-j` | Number of parallel downloads (default: 1) |
| `--retry-max` | Max retry attempts per download (default: 5) |
| `--retry-delay` | Initial retry delay in seconds (default: 5) |
| `--wait-for-network`, `-w` | Wait indefinitely for network on failure |
| `--min-space` | Minimum free disk space in MB (default: 1024) |
| `--no-preflight` | Skip disk space check before downloading |
| `--cancel ID` | Cancel a queued download by ID |
| `--clear` | Clear completed downloads from queue |

## Model Sources

### HuggingFace

Format: `user/repo[:quant]`

The quant tag is optional and case-insensitive. If omitted, defaults to Q4_K_M or the first GGUF file found.

```bash
llama-download bartowski/Llama-3.2-1B-Instruct-GGUF:q4_k_m
llama-download -hf unsloth/phi-4-GGUF
```

### Docker Hub / OCI Registry

Format: `[registry/]model[:tag]`

```bash
llama-download -dr smollm2:135M-Q4_0
llama-download -dr ai/gemma3:latest
```

### Direct URL

Any HTTPS URL to a GGUF file:

```bash
llama-download https://example.com/model.gguf
```

## Cache Location

Models are downloaded to the llama.cpp cache directory:

| Platform | Location |
|----------|----------|
| Windows | `%LOCALAPPDATA%\llama.cpp\` |
| Linux | `~/.cache/llama.cpp/` |
| macOS | `~/Library/Caches/llama.cpp/` |

Override with the `LLAMA_CACHE` environment variable.

## Queue Persistence

Download state is saved to `download-queue.json` in the cache directory. If a download is interrupted:

1. The partial file is saved as `.downloadInProgress`
2. Queue state is preserved
3. Run `llama-download --resume` to continue

## Resume Support

Downloads can be resumed if the server supports HTTP Range requests:

- **HuggingFace**: Fully supported
- **Docker Hub**: Fully supported (OCI blob endpoints support Range)
- **Direct URLs**: Depends on the server

If a download is interrupted and the server doesn't support resume, the file will be re-downloaded from scratch.

## Update Checking

The `--update` flag scans cached models and re-downloads any that have changed (based on ETag):

```bash
llama-download --update
```

This detects:
- Docker models via manifest files
- HuggingFace models via `.etag` files

## Examples

### Batch Download Script

Create a `models.txt` file:
```
# Lines starting with # are comments
bartowski/Llama-3.2-1B-Instruct-GGUF:q4_k_m
unsloth/phi-4-GGUF:q6_k
ggml-org/gemma-3-12b-it-qat-GGUF
```

Then run:
```bash
llama-download -f models.txt --wait-for-network
```

### Scheduled Updates

Check for updates to all cached models:
```bash
llama-download --update --dry-run  # Preview
llama-download --update            # Download updates
```
