#!/usr/bin/env bash
#
# build-llamafile.sh - Fetch llamafile tools and build a llamafile with a model
#
# This script downloads the llamafile tooling from Mozilla and creates a
# self-contained executable that bundles an LLM model with the inference engine.
#
# Usage:
#   ./scripts/build-llamafile.sh [OPTIONS]
#
# Options:
#   -m, --model PATH       Path to a GGUF model file (required if not using --download-model)
#   -d, --download-model   Download a default small model for testing
#   -u, --model-url URL    URL to download a specific GGUF model
#   -o, --output NAME      Output filename (default: model.llamafile)
#   -v, --version VER      Llamafile version to use (default: latest)
#   -c, --cache-dir DIR    Cache directory for downloads (default: .llamafile-cache)
#   -s, --server           Build as server mode (includes web UI)
#   -a, --args "ARGS"      Default arguments to embed in the llamafile
#   -h, --help             Show this help message
#
# Examples:
#   # Build with a local model
#   ./scripts/build-llamafile.sh -m ./models/llama-2-7b.Q4_K_M.gguf -o llama2.llamafile
#
#   # Download a small test model and build
#   ./scripts/build-llamafile.sh --download-model -o test.llamafile
#
#   # Build a server-mode llamafile
#   ./scripts/build-llamafile.sh -m model.gguf -s -o server.llamafile
#
# Requirements:
#   - curl or wget
#   - unzip
#   - chmod
#
# License: MIT
#

set -euo pipefail

# =============================================================================
# Configuration
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Defaults
LLAMAFILE_VERSION="latest"
CACHE_DIR="${PROJECT_ROOT}/.llamafile-cache"
OUTPUT_NAME="model.llamafile"
MODEL_PATH=""
MODEL_URL=""
DOWNLOAD_MODEL=false
SERVER_MODE=false
EMBED_ARGS=""

# URLs
LLAMAFILE_RELEASES_URL="https://github.com/Mozilla-Ocho/llamafile/releases"
DEFAULT_TEST_MODEL_URL="https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"
DEFAULT_TEST_MODEL_NAME="tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# =============================================================================
# Helper Functions
# =============================================================================

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1" >&2
}

die() {
    log_error "$1"
    exit 1
}

show_help() {
    head -50 "$0" | grep -E '^#' | sed 's/^# \?//'
    exit 0
}

check_command() {
    command -v "$1" &> /dev/null
}

# Download a file with retry support
download_file() {
    local url="$1"
    local output="$2"
    local max_retries=4
    local retry_delay=2

    for ((i=1; i<=max_retries; i++)); do
        log_info "Downloading: $url (attempt $i/$max_retries)"

        if check_command curl; then
            if curl -fSL --progress-bar -o "$output" "$url"; then
                return 0
            fi
        elif check_command wget; then
            if wget -q --show-progress -O "$output" "$url"; then
                return 0
            fi
        else
            die "Neither curl nor wget found. Please install one of them."
        fi

        if [ $i -lt $max_retries ]; then
            log_warn "Download failed, retrying in ${retry_delay}s..."
            sleep $retry_delay
            retry_delay=$((retry_delay * 2))
        fi
    done

    die "Failed to download $url after $max_retries attempts"
}

# Get the latest llamafile release version
get_latest_version() {
    local version

    if check_command curl; then
        version=$(curl -fsSL "https://api.github.com/repos/Mozilla-Ocho/llamafile/releases/latest" | \
                  grep '"tag_name"' | sed -E 's/.*"([^"]+)".*/\1/')
    elif check_command wget; then
        version=$(wget -qO- "https://api.github.com/repos/Mozilla-Ocho/llamafile/releases/latest" | \
                  grep '"tag_name"' | sed -E 's/.*"([^"]+)".*/\1/')
    else
        die "Neither curl nor wget found"
    fi

    echo "$version"
}

# Detect the current platform
detect_platform() {
    local os arch

    case "$(uname -s)" in
        Linux*)  os="linux" ;;
        Darwin*) os="macos" ;;
        MINGW*|MSYS*|CYGWIN*) os="windows" ;;
        *)       die "Unsupported operating system: $(uname -s)" ;;
    esac

    case "$(uname -m)" in
        x86_64|amd64)  arch="amd64" ;;
        aarch64|arm64) arch="arm64" ;;
        *)             die "Unsupported architecture: $(uname -m)" ;;
    esac

    echo "${os}-${arch}"
}

# =============================================================================
# Main Functions
# =============================================================================

parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            -m|--model)
                MODEL_PATH="$2"
                shift 2
                ;;
            -d|--download-model)
                DOWNLOAD_MODEL=true
                shift
                ;;
            -u|--model-url)
                MODEL_URL="$2"
                shift 2
                ;;
            -o|--output)
                OUTPUT_NAME="$2"
                shift 2
                ;;
            -v|--version)
                LLAMAFILE_VERSION="$2"
                shift 2
                ;;
            -c|--cache-dir)
                CACHE_DIR="$2"
                shift 2
                ;;
            -s|--server)
                SERVER_MODE=true
                shift
                ;;
            -a|--args)
                EMBED_ARGS="$2"
                shift 2
                ;;
            -h|--help)
                show_help
                ;;
            *)
                die "Unknown option: $1. Use --help for usage information."
                ;;
        esac
    done
}

validate_args() {
    # Check if we have a model source
    if [ -z "$MODEL_PATH" ] && [ -z "$MODEL_URL" ] && [ "$DOWNLOAD_MODEL" = false ]; then
        die "No model specified. Use -m, -u, or -d to specify a model source."
    fi

    # If model path specified, check it exists
    if [ -n "$MODEL_PATH" ] && [ ! -f "$MODEL_PATH" ]; then
        die "Model file not found: $MODEL_PATH"
    fi

    # Ensure output has .llamafile extension
    if [[ "$OUTPUT_NAME" != *.llamafile ]]; then
        OUTPUT_NAME="${OUTPUT_NAME}.llamafile"
    fi
}

setup_cache_dir() {
    mkdir -p "$CACHE_DIR"
    log_info "Using cache directory: $CACHE_DIR"
}

fetch_llamafile_tools() {
    log_info "Fetching llamafile tools..."

    # Determine version
    if [ "$LLAMAFILE_VERSION" = "latest" ]; then
        log_info "Checking for latest llamafile version..."
        LLAMAFILE_VERSION=$(get_latest_version)
        log_info "Latest version: $LLAMAFILE_VERSION"
    fi

    local platform
    platform=$(detect_platform)
    log_info "Detected platform: $platform"

    # Download zipalign (required for creating llamafiles)
    local zipalign_path="${CACHE_DIR}/zipalign"
    if [ ! -f "$zipalign_path" ]; then
        local zipalign_url="${LLAMAFILE_RELEASES_URL}/download/${LLAMAFILE_VERSION}/zipalign"
        download_file "$zipalign_url" "$zipalign_path"
        chmod +x "$zipalign_path"
        log_success "Downloaded zipalign"
    else
        log_info "Using cached zipalign"
    fi

    # Download llamafile base executable
    local llamafile_base="${CACHE_DIR}/llamafile-${LLAMAFILE_VERSION}"
    if [ ! -f "$llamafile_base" ]; then
        local llamafile_url="${LLAMAFILE_RELEASES_URL}/download/${LLAMAFILE_VERSION}/llamafile-${LLAMAFILE_VERSION}"
        download_file "$llamafile_url" "$llamafile_base"
        chmod +x "$llamafile_base"
        log_success "Downloaded llamafile base executable"
    else
        log_info "Using cached llamafile base executable"
    fi

    # If server mode, also get llamafile-server
    if [ "$SERVER_MODE" = true ]; then
        local server_base="${CACHE_DIR}/llamafile-server-${LLAMAFILE_VERSION}"
        if [ ! -f "$server_base" ]; then
            local server_url="${LLAMAFILE_RELEASES_URL}/download/${LLAMAFILE_VERSION}/llamafile-server-${LLAMAFILE_VERSION}"
            download_file "$server_url" "$server_base"
            chmod +x "$server_base"
            log_success "Downloaded llamafile-server base executable"
        else
            log_info "Using cached llamafile-server base executable"
        fi
    fi
}

download_model() {
    local url="${MODEL_URL:-$DEFAULT_TEST_MODEL_URL}"
    local model_name

    # Extract filename from URL
    model_name=$(basename "$url")
    MODEL_PATH="${CACHE_DIR}/${model_name}"

    if [ -f "$MODEL_PATH" ]; then
        log_info "Using cached model: $MODEL_PATH"
        return
    fi

    log_info "Downloading model: $model_name"
    download_file "$url" "$MODEL_PATH"
    log_success "Downloaded model to: $MODEL_PATH"
}

build_llamafile() {
    log_info "Building llamafile..."

    local zipalign="${CACHE_DIR}/zipalign"
    local base_exe

    if [ "$SERVER_MODE" = true ]; then
        base_exe="${CACHE_DIR}/llamafile-server-${LLAMAFILE_VERSION}"
    else
        base_exe="${CACHE_DIR}/llamafile-${LLAMAFILE_VERSION}"
    fi

    local output_path="${PROJECT_ROOT}/${OUTPUT_NAME}"

    # Copy base executable
    cp "$base_exe" "$output_path"

    # Create .args file if arguments specified
    local args_file=""
    if [ -n "$EMBED_ARGS" ]; then
        args_file=$(mktemp)
        echo "$EMBED_ARGS" > "$args_file"
    fi

    # Use zipalign to append the model
    log_info "Appending model to llamafile..."

    # Create a temporary directory for the zip structure
    local temp_dir
    temp_dir=$(mktemp -d)
    local model_basename
    model_basename=$(basename "$MODEL_PATH")

    # Copy model to temp dir
    cp "$MODEL_PATH" "${temp_dir}/${model_basename}"

    # If we have args, add them too
    if [ -n "$args_file" ]; then
        cp "$args_file" "${temp_dir}/.args"
        rm "$args_file"
    fi

    # Create the zip and append
    (cd "$temp_dir" && zip -r -0 model.zip .)

    # Append the zip to the executable
    cat "${temp_dir}/model.zip" >> "$output_path"

    # Use zipalign to fix the zip offsets
    "$zipalign" -j0 "$output_path" || {
        log_warn "zipalign reported issues, but the llamafile may still work"
    }

    # Cleanup
    rm -rf "$temp_dir"

    # Make executable
    chmod +x "$output_path"

    log_success "Built llamafile: $output_path"

    # Print file size
    local size
    size=$(du -h "$output_path" | cut -f1)
    log_info "File size: $size"
}

print_usage_instructions() {
    echo ""
    echo "=============================================="
    echo "  Llamafile created successfully!"
    echo "=============================================="
    echo ""
    echo "To run your llamafile:"
    echo ""
    echo "  ./${OUTPUT_NAME}"
    echo ""
    if [ "$SERVER_MODE" = true ]; then
        echo "This will start a web server. Open http://localhost:8080 in your browser."
    else
        echo "This will start an interactive chat session."
    fi
    echo ""
    echo "Additional options:"
    echo "  ./${OUTPUT_NAME} --help           Show all available options"
    echo "  ./${OUTPUT_NAME} -p \"prompt\"      Run with a specific prompt"
    echo "  ./${OUTPUT_NAME} -n 100           Generate 100 tokens"
    echo "  ./${OUTPUT_NAME} --server         Run in server mode"
    echo ""
    echo "Note: On macOS, you may need to allow the executable in"
    echo "      System Preferences > Security & Privacy."
    echo ""
}

main() {
    echo "========================================"
    echo "  Llamafile Build Script"
    echo "========================================"
    echo ""

    parse_args "$@"
    validate_args
    setup_cache_dir

    # Fetch llamafile tools
    fetch_llamafile_tools

    # Download model if requested
    if [ "$DOWNLOAD_MODEL" = true ] || [ -n "$MODEL_URL" ]; then
        download_model
    fi

    # Build the llamafile
    build_llamafile

    # Print instructions
    print_usage_instructions

    log_success "Done!"
}

# =============================================================================
# Entry Point
# =============================================================================

main "$@"
