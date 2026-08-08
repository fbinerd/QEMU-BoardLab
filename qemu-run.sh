#!/usr/bin/env bash
set -euo pipefail

exp="${1:?usage: qemu-run.sh <experiment-dir-name>}"
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
elf="$repo_dir/out/$exp/out.elf"

if [[ ! -f "$elf" ]]; then
    echo "error: $elf not built yet - run build.sh first" >&2
    exit 1
fi

status=0
timeout 10 qemu-system-arm \
    -M virt \
    -cpu cortex-a15 \
    -m 64M \
    -nographic \
    -semihosting \
    -kernel "$elf" || status=$?

# QEMU's semihosting SYS_EXIT (ADP_Stopped_ApplicationExit) does not
# propagate the guest's exit code as the qemu process's own exit status -
# results are read from the printed PASS/FAIL text, not $?. The only
# exit status worth treating as a real failure here is timeout's 124
# (the guest never reached sh_exit at all).
if [[ $status -eq 124 ]]; then
    echo "error: qemu timed out - guest never called sh_exit" >&2
    exit 124
fi
exit 0
