#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${IM7CAM_RECOVERY_OUT:-${SCRIPT_DIR}/output}"
INIT_BINARY="${IM7CAM_INIT_BINARY:-$OUT_DIR/init}"
GEN_INIT_CPIO="${IM7CAM_GEN_INIT_CPIO:-}"

for tool in install sha256sum; do
    command -v "$tool" >/dev/null || {
        echo "error: required tool not found: $tool" >&2
        exit 1
    }
done

[[ -x "$INIT_BINARY" ]] || {
    echo "error: missing recovery init binary: $INIT_BINARY" >&2
    exit 1
}
[[ -x "$GEN_INIT_CPIO" ]] || {
    echo "error: set IM7CAM_GEN_INIT_CPIO to the kernel gen_init_cpio tool" >&2
    exit 1
}

mkdir -p "$OUT_DIR"
raw_image="$OUT_DIR/im7-recovery-initramfs.cpio"
(cd "$SCRIPT_DIR" && "$GEN_INIT_CPIO" -t 0 initramfs.list) > "$raw_image"

sha256sum "$raw_image"
echo "Built: $raw_image"
