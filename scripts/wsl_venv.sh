#!/bin/bash
# Makes the development environment in WSL or Linux: ~/venvs/four-eyes with requirements-dev.txt.
set -euo pipefail
cd "$(dirname "$0")/.."
test -x ~/venvs/four-eyes/bin/python || python3 -m venv ~/venvs/four-eyes
~/venvs/four-eyes/bin/pip install -q -r requirements-dev.txt
~/venvs/four-eyes/bin/python -c 'import pytest, hypothesis, fastapi; print("environment ready")'
