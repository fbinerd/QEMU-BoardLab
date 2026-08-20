#!/bin/bash
# Boots the extracted im7 (Imou IPC-S21F) U-Boot partition in the im7cam
# QEMU skeleton machine. This is a bringup tool, not a working boot yet -
# see BRINGUP-NOTES.md for what's confirmed vs. guessed. Expect it to
# print the "im7cam: unimplemented ..." trace log and then likely hang or
# crash - that log IS the point right now (see board/im7cam.c's own
# comment on why reads deliberately return 0).
#
# Usage:
#   ./run.sh [path-to-0_U-Boot.bin]
#
# With no argument, looks for images/0_U-Boot.bin (gitignored - copy it
# in yourself from the source investigation, see BRINGUP-NOTES.md
# "Where the firmware came from").
#
# -d unimp: makes the logging stub's trace actually visible (LOG_UNIMP is
# silent by default). Ctrl-A X quits QEMU.

set -euo pipefail

IMAGE=im7cam-qemu:9.1.0
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

KERNEL="${1:-${SCRIPT_DIR}/images/0_U-Boot.bin}"

if [[ ! -f "$KERNEL" ]]; then
    echo "error: no U-Boot image found." >&2
    echo "usage: $0 [path-to-0_U-Boot.bin]" >&2
    echo "default location: ${SCRIPT_DIR}/images/0_U-Boot.bin (gitignored)" >&2
    exit 1
fi

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "Building ${IMAGE} (first run only - vendors its own QEMU copy)..."
    docker build -t "$IMAGE" "$SCRIPT_DIR"
fi

KERNEL_DIR="$(cd "$(dirname "$KERNEL")" && pwd)"
KERNEL_FILE="$(basename "$KERNEL")"

echo "Booting ${KERNEL} in the im7cam skeleton machine."
echo "Console below IS the UART - real output not expected yet (see BRINGUP-NOTES.md)."
echo "Ctrl-A X to quit."
echo

exec docker run --rm -it \
    -v "${KERNEL_DIR}:/fw:ro" \
    "$IMAGE" \
    /build/qemu-9.1.0/build/qemu-system-arm -M im7cam -nographic -monitor none \
    -serial stdio -d unimp \
    -kernel "/fw/${KERNEL_FILE}"
