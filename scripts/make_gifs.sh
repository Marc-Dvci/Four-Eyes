#!/bin/bash
# Records the car page and cuts the README GIFs from the recordings.
#   scripts/make_gifs.sh
set -euo pipefail
cd "$(dirname "$0")/.."
PY="${PY:-python}"
"$PY" scripts/record_page.py
for name in message-attack unlock-80; do
    video=$(ls -t build/rec/$name/*.webm | head -1)
    "$PY" scripts/make_gif.py "$video" "docs/$name.gif" --start 1.0 --duration 6 --width 900 --fps 10
done
ls -l docs/*.gif
