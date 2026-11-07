#!/bin/bash
# The repository checks CI runs after the tests: placeholders, local links, spelling.
cd "$(dirname "$0")/.."
BIN="${BIN:-$HOME/venvs/four-eyes/bin}"
"$BIN/python" scripts/check_placeholders.py; echo "placeholders rc=$?"
"$BIN/python" scripts/check_links.py | tail -5; echo "links rc=${PIPESTATUS[0]}"
"$BIN/codespell"; echo "codespell rc=$?"
