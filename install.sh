#!/bin/bash
set -euo pipefail

function has_command() {
  command -v "$1" >/dev/null 2>&1
}

function is_macos() {
  [[ "$(uname -s)" == "Darwin" ]]
}

function is_linux() {
  [[ "$(uname -s)" == "Linux" ]]
}

function is_wsl() {
  [[ -f /proc/version ]] && grep -q Microsoft /proc/version
}

function fail() {
  echo "$1" >&2
  exit 1
}

function install_build_tools() {
    if has_command apt-get ; then
        sudo apt-get update
        sudo apt-get install -y build-essential cmake libcurl4-openssl-dev ccache
    elif has_command yum ; then
        sudo yum install -y cmake gcc gcc-c++ curl-devel
    elif has_command dnf ; then
        sudo dnf install -y cmake gcc gcc-c++ curl-devel
    elif has_command brew ; then
        brew install cmake gcc curl ccache
    elif has_command nix ; then
        nix-shell -p cmake gcc curl ccache
    else
        echo "Please install a package manager that can install cmake and gcc"
        fail
    fi
}

function install_from_brew() {
    brew install llama.cpp
}

function install_from_nix() {
    # TODO: unstable channel?
    # https://discourse.nixos.org/t/installing-only-a-single-package-from-unstable/5598
    nix-shell -p llama-cpp
}

function install_from_sources() {
    local dir=$(mktemp -d)

    # Trap exit
    trap 'rm -rf "$dir"' EXIT

    git clone --depth=0 --recursive https://github.com/ggerganov/llama.git "$dir"
    cd "$dir"

    local generator="Unix Makefiles"
    if has_command ninja ; then
        generator="Ninja"
    fi
    cmake -B build -G "$generator" -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release --parallel
    if has_command sudo ; then
        sudo cmake --install build
    else
        cmake --install build
    fi
}

# function has

if is_macos; then
    if has_command brew ; then
        install_from_brew
    elif has_command nix ; then
        install_from_nix
    else
        echo "Please install Homebrew or Nix package manager"
        echo ""
        echo "To install Homebrew (preferred), just run:"
        echo ""
        echo '  /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)'
        echo ""
        echo "To install nix, run:"
        echo ""
        echo "  curl -L https://nixos.org/nix/install | sh"
        echo ""
        fail
    fi
elif is_wsl; then
    # if has_command choco ; then
    #     # TODO: choco repo!
    # endif
elif has_command nix; then
    ni-shell -p llama-cpp 
else
    if ! has_command cmake || ! has_command gcc ; then
        install_build_tools
    fi
    install_from_sources
fi

which llama-cli
which llama-server

echo "llama has been installed successfully"
