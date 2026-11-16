#!/bin/bash
# Prints the number of tests of each kind, the totals, and both coverage figures, as the README quotes them.
set -uo pipefail
cd "$(dirname "$0")/.."
PY="${PY:-python}"
count() { "$PY" -m pytest --collect-only -q -p no:randomly -m "$1" 2>/dev/null | tail -n 1 | grep -Eo '^[0-9]+'; }
echo "unit $(count 'not integration and not e2e and not live') · integration $(count integration) · end-to-end $(count e2e)"
"$PY" -m pytest -q --cov 2>&1 | grep -E 'TOTAL|passed'
PY="$PY" bash scripts/coverage_core.sh 2>&1 | grep -E '^(lines|branches)'
"$PY" -m gcovr --root . --filter 'qnx/core/gate_core.c' --object-directory build/host build/host 2>/dev/null | grep gate_core
"$PY" -m gcovr --root . --filter 'qnx/core/gate_core.c' --object-directory build/host --txt-metric branch build/host 2>/dev/null | grep gate_core
