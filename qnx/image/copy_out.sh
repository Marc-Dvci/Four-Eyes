#!/bin/bash
# Copies a built image (ifs.bin, disk-qemu.vmdk, disk-qemu) to a directory the Windows QEMU can read.
#
#   qnx/image/copy_out.sh IMAGE_DIR DEST_DIR
set -euo pipefail
SRC="${1:?image directory}/output"
DEST="${2:?destination}"
mkdir -p "$DEST"
cp "$SRC/ifs.bin" "$SRC/disk-qemu.vmdk" "$DEST/"
cp --sparse=always "$SRC/disk-qemu" "$DEST/disk-qemu"
ls -la "$DEST"
