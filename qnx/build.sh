#!/bin/bash
# Builds Four Eyes' QNX programs into ~/Four-Eyes-work/build/x86_64.
#
#   qnx/build.sh [make targets...]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HOME/qnx800/qnxsdp-env.sh" >/dev/null
make -C "$HERE" -j 4 "$@"
