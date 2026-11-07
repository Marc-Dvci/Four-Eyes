#!/bin/bash
# Builds Four Eyes' QNX 8.0 image for QEMU x86_64 with mkqnximage.
#
#   qnx/image/build.sh IMAGE_DIR --secpol=no|open|secure [--policy=FILE]
#
#   --secpol=open    unrestricted, with secpolgenerate recording what every process uses (to draft a policy)
#   --secpol=secure  enforce the policy given with --policy (qnx/image/four-eyes.secpol)
#   --secpol=no      no policy at all: the "kernel off" comparison
#
# Run qnx/build.sh first; the programs come from ~/Four-Eyes-work/build/x86_64 and the model from
# ~/Four-Eyes-work/models. The image directory must be new or one this script made before.
#
# startup-x86 -f 0,0,200M: the Windows Hypervisor Platform does not report its APIC timer frequency in a form the
# startup library reads. -c advice:0x2 declares CPU 1 as a cluster of its own (the guest has two CPUs; a cluster
# naming CPUs the guest does not have stops the kernel at boot). FE_STARTUP_OPTS replaces both. procnto -P 22: priorities 22 and above need the
# priority ability, which only the body controller holds, so the agent cannot outrank its 20 ms cycle at 30.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
IMAGE_DIR="${1:?image directory}"
shift
BUILD="${FE_BUILD:-$HOME/Four-Eyes-work/build/x86_64}"
MODEL="${FE_MODEL:-$HOME/Four-Eyes-work/models/qwen2.5-1.5b-instruct-q4_k_m.gguf}"
source "$HOME/qnx800/qnxsdp-env.sh" >/dev/null
for p in vcand bodyd gated agentd fe-bridge fe-canfuzz; do test -x "$BUILD/$p" || { echo "missing $BUILD/$p"; exit 1; }; done
test -s "$MODEL" || { echo "missing model $MODEL"; exit 1; }
mkdir -p "$IMAGE_DIR/local/snippets"
for f in "$HERE"/snippets/*.custom; do
    sed -e "s#__FE_BUILD__#$BUILD#g" -e "s#__FE_MODEL__#$MODEL#g" "$f" >"$IMAGE_DIR/local/snippets/$(basename "$f")"
done
printf '#define __STARTUP_OPTS__ %s\n' "${FE_STARTUP_OPTS:--c advice:0x2 -f 0,0,200M}" >"$IMAGE_DIR/local/snippets/definitions.custom"
KEY="${FE_SSH_KEY:-$HERE/../../.guest/ssh/id_ed25519}"
mkdir -p "$(dirname "$KEY")"
test -f "$KEY" || ssh-keygen -q -t ed25519 -N '' -C four-eyes-guest -f "$KEY"
cd "$IMAGE_DIR"
mkqnximage --noprompt --force --build --type=qemu --arch=x86_64 --graphics=no --hostname=foureyes --cpu=2 \
    --ram=4G --sys-size=32 --data-size=1400 --proc="procnto-smp-instr -P 22" --ssh-ident="$KEY.pub" "$@"
ls -la output/ifs.bin output/disk-qemu.vmdk output/disk-qemu
