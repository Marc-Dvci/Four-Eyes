#!/bin/bash
# Breaks the gate's core one rule at a time and checks that the tests go red each time, then restores the file.
#
#   scripts/mutants.sh
set -uo pipefail
cd "$(dirname "$0")/.."
PY="${PY:-$HOME/venvs/four-eyes/bin/python}"
F=qnx/core/gate_core.c
cp "$F" /tmp/fe_gate_core.orig
mutants=(
    's/speed_dkmh > g->lim.stationary_dkmh/speed_dkmh >= g->lim.stationary_dkmh/'
    's/p->value == 0 \&\& !parked(g, true)/p->value == 0 \&\& !parked(g, false)/'
    's/p->value == 1 \&\& !parked(g, false)/p->value == 1 \&\& !parked(g, true)/'
    's/g->vehicle_ms > g->lim.state_max_age_ms/g->vehicle_ms > g->lim.state_max_age_ms + 1/'
    's/tokens_milli < 1000u) return false/tokens_milli < 1u) return false/'
    's/g->turn_used >= g->lim.turn_budget/g->turn_used > g->lim.turn_budget/'
    's/turn_origin != FE_ORIGIN_DRIVER/turn_origin == FE_ORIGIN_NONE/'
    's/p->value > g->lim.window_fast_max/p->value > g->lim.window_fast_max + 1/'
    's/p->value < row->min || p->value > row->max/p->value < row->min/'
    's/if (!row->writable) return refuse(FE_R_NOT_ALLOWED);//'
)
killed=0
for m in "${mutants[@]}"; do
    sed -i "$m" "$F"
    if cmp -s "$F" /tmp/fe_gate_core.orig; then
        echo "NOT APPLIED  $m"
    elif "$PY" -m pytest -q -x -p no:randomly tests/test_core.py tests/test_cases.py >/dev/null 2>&1; then
        echo "SURVIVED     $m"
    else
        echo "killed       $m"
        killed=$((killed + 1))
    fi
    cp /tmp/fe_gate_core.orig "$F"
done
echo "$killed of ${#mutants[@]} mutants killed"
