#!/bin/bash
# Line and branch coverage of the gate's decision core (the C files in qnx/core), measured with gcov while the test
# suite drives them through app/fecore.py. Fails below the floor.
#
#   scripts/coverage_core.sh [floor_percent]     (default 90)
set -euo pipefail
cd "$(dirname "$0")/.."
PY="${PY:-python}"
FLOOR="${1:-90}"
find build/host -name '*.gcda' -delete 2>/dev/null || true
FE_COVERAGE=1 "$PY" -m pytest -q -p no:randomly -m "not e2e" tests/ >/dev/null
"$PY" -m gcovr --root . --filter 'qnx/core/' --object-directory build/host --txt-metric branch --print-summary \
    --fail-under-line "$FLOOR" --fail-under-branch 70 --markdown-summary build/host/coverage.md build/host
