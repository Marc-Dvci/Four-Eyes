#!/bin/bash
# Fetches llama.cpp at a pinned upstream revision and builds its static libraries for QNX 8.0 x86_64
# (AVX2, no AVX-512, no OpenMP).
#
#   qnx/llm/build_llama.sh [SRC_DIR] [BUILD_DIR]
#
# SRC_DIR is where the pinned upstream tree is fetched (default ~/Four-Eyes-work/llama.cpp-<rev>). It is created if
# missing and checked against the pinned revision if present. Any patch in qnx/llm/patches/ is applied in name order.
# Only libllama and libggml are built: the agent links them directly, so no llama.cpp tool runs in the guest.
#
# Outputs, under BUILD_DIR:
#   src/libllama.a  ggml/src/libggml.a  ggml/src/libggml-base.a  ggml/src/libggml-cpu.a
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
LLAMA_URL="https://github.com/ggml-org/llama.cpp.git"
LLAMA_REV="aa94f2086147cd4bfbcff818ce998dffce5fcb39"
SRC="${1:-$HOME/Four-Eyes-work/llama.cpp-${LLAMA_REV:0:9}}"
BUILD="${2:-$HOME/Four-Eyes-work/build/llama-qnx-${LLAMA_REV:0:9}}"

if [ ! -d "$SRC/.git" ]; then
    git init -q "$SRC"
    git -C "$SRC" remote add origin "$LLAMA_URL"
    git -C "$SRC" fetch -q --depth 1 origin "$LLAMA_REV"
    git -C "$SRC" checkout -q FETCH_HEAD
fi
have="$(git -C "$SRC" rev-parse HEAD)"
if [ "$have" != "$LLAMA_REV" ]; then
    echo "build_llama: $SRC is at $have, expected $LLAMA_REV" >&2
    exit 1
fi
shopt -s nullglob
for p in "$HERE"/patches/*.patch; do
    if git -C "$SRC" apply --reverse --check "$p" 2>/dev/null; then
        echo "build_llama: already applied $(basename "$p")"
    else
        git -C "$SRC" apply "$p"
        echo "build_llama: applied $(basename "$p")"
    fi
done

source "$HOME/qnx800/qnxsdp-env.sh" >/dev/null
cmake -S "$SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$HERE/toolchain-qnx-x86_64.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DGGML_NATIVE=OFF -DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON -DGGML_AVX512=OFF \
    -DGGML_OPENMP=OFF -DLLAMA_CURL=OFF \
    -DLLAMA_BUILD_COMMON=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_TOOLS=OFF -DLLAMA_BUILD_EXAMPLES=OFF \
    -DLLAMA_BUILD_SERVER=OFF -DLLAMA_BUILD_APP=OFF
cmake --build "$BUILD" -j 4 --target llama ggml
for lib in src/libllama.a ggml/src/libggml.a ggml/src/libggml-base.a ggml/src/libggml-cpu.a; do
    if [ ! -f "$BUILD/$lib" ]; then
        echo "build_llama: missing $BUILD/$lib" >&2
        exit 1
    fi
    echo "build_llama: $lib $(stat -c %s "$BUILD/$lib") bytes"
done
