#!/bin/bash
# Builds llama.cpp's static libraries for QNX 8.0 x86_64 (AVX2, no AVX-512, no OpenMP).
#
#   qnx/llm/build_llama.sh [LLAMA_SRC] [BUILD_DIR]
#
# LLAMA_SRC is a llama.cpp tree that builds for QNX (default ~/Four-Eyes-work/llama.cpp). Only libllama and
# libggml are built: the agent links them directly, so no llama.cpp tool runs in the guest.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${1:-$HOME/Four-Eyes-work/llama.cpp}"
BUILD="${2:-$HOME/Four-Eyes-work/build/llama-qnx}"
source "$HOME/qnx800/qnxsdp-env.sh" >/dev/null
cmake -S "$SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$HERE/toolchain-qnx-x86_64.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DGGML_NATIVE=OFF -DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON -DGGML_AVX512=OFF \
    -DGGML_OPENMP=OFF -DLLAMA_CURL=OFF \
    -DLLAMA_BUILD_COMMON=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_TOOLS=OFF -DLLAMA_BUILD_EXAMPLES=OFF \
    -DLLAMA_BUILD_SERVER=OFF -DLLAMA_BUILD_APP=OFF
cmake --build "$BUILD" -j 4 --target llama ggml
find "$BUILD" -name "*.a" -printf "%p %s\n"
