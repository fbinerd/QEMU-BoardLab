#!/bin/bash
set -e
/build/qemu-9.1.0/build/qemu-system-arm -M mr80x -nographic -monitor none \
  -serial null -nic none -kernel /fw/appsbl.unpadded.elf -s &
QEMU_PID=$!
sleep 1

echo "=== reading #1 ==="
gdb-multiarch -batch \
  -ex "target remote localhost:1234" \
  -ex "x/1xw 0x4A2000" \
  -ex "detach" \
  /fw/appsbl.unpadded.elf 2>&1 | grep "0x4a2000"
date +%s.%N

sleep 3

echo "=== reading #2 ==="
gdb-multiarch -batch \
  -ex "target remote localhost:1234" \
  -ex "x/1xw 0x4A2000" \
  -ex "detach" \
  /fw/appsbl.unpadded.elf 2>&1 | grep "0x4a2000"
date +%s.%N

kill -9 $QEMU_PID 2>/dev/null || true
wait $QEMU_PID 2>/dev/null || true
