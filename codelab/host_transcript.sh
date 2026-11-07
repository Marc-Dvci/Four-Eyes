#!/bin/bash
# Runs the codelab's host commands and writes what they printed to codelab/transcripts/host.txt.
#   bash codelab/host_transcript.sh DRAFT_POLICY
cd "$(dirname "$0")/.."
DRAFT="${1:?the policy secpolgenerate wrote}"
source "$HOME/qnx800/qnxsdp-env.sh" >/dev/null
OUT=codelab/transcripts/host.txt
run() { echo "# $*"; bash -c "$*" 2>&1; echo; }
T=$(mktemp -d)
cp qnx/image/four-eyes.secpol "$T/four-eyes.secpol"
cp "$DRAFT" "$T/draft.secpol"
# the shipped policy without the gain_priv block the shell needs to start the bridge
python3 - "$T" <<'EOF'
import sys, re
t = sys.argv[1]
p = open(f"{t}/four-eyes.secpol").read()
p = re.sub(r"allow user_root_t self:ability \{\n    gain_priv:\n.*?\};\n", "", p, flags=re.S)
open(f"{t}/no-gain.secpol", "w").write(p)
EOF
{
    run "which secpolcompile"
    run "grep -c '' $T/draft.secpol"
    run "sed -n '/Rules for type agent_t/,/Rules for type body_t/p' $T/draft.secpol"
    run "python3 tools/check_policy.py $T/draft.secpol"
    run "python3 tools/check_policy.py qnx/image/four-eyes.secpol"
    run "secpolcompile -o $T/secpol.bin $T/four-eyes.secpol && ls -l $T/secpol.bin | awk '{print \$5, \$9}'"
    run "secpolcompile -o $T/bad.bin $T/no-gain.secpol"
} | sed "s#$T#/tmp/codelab#g" > "$OUT"
cat "$OUT"
