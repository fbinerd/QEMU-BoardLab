#!/bin/bash
set -e
/build/qemu-9.1.0/build/qemu-system-arm -M mr80x -nographic -monitor none \
  -serial null -kernel /fw/appsbl.unpadded.elf -s -S &
QEMU_PID=$!
sleep 1

gdb-multiarch -batch \
  -ex "target remote localhost:1234" \
  -ex "break *0x4a922144" \
  -ex "continue" \
  -ex "info registers" \
  -ex "x/8xw \$sp" \
  -ex "x/4i \$pc" \
  -ex "bt" \
  /fw/appsbl.unpadded.elf

kill $QEMU_PID 2>/dev/null || true
wait $QEMU_PID 2>/dev/null || true
