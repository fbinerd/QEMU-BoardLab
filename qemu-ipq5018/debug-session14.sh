#!/bin/bash
set -e
/build/qemu-9.1.0/build/qemu-system-arm -M mr80x -nographic -monitor none \
  -serial null -nic none -kernel /fw/appsbl.unpadded.elf -s -S &
QEMU_PID=$!
sleep 2

timeout 40 gdb-multiarch -batch -x /gdbcmds2.txt /fw/appsbl.unpadded.elf 2>&1

kill -9 $QEMU_PID 2>/dev/null || true
wait $QEMU_PID 2>/dev/null || true
