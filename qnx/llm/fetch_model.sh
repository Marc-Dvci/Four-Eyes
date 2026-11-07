#!/bin/bash
# Downloads the agent's model: Qwen2.5-1.5B-Instruct, Q4_K_M GGUF, from Qwen's own Hugging Face repository.
# Licence: Apache 2.0 (https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct/blob/main/LICENSE).
#
#   qnx/llm/fetch_model.sh [DEST_DIR]
set -euo pipefail
DEST="${1:-$HOME/Four-Eyes-work/models}"
NAME=qwen2.5-1.5b-instruct-q4_k_m.gguf
URL="https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct-GGUF/resolve/main/$NAME"
mkdir -p "$DEST"
if [ ! -s "$DEST/$NAME" ]; then
    curl -fL --retry 3 -o "$DEST/$NAME.part" "$URL"
    mv "$DEST/$NAME.part" "$DEST/$NAME"
fi
ls -l "$DEST/$NAME"
sha256sum "$DEST/$NAME"
