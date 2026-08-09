#!/bin/bash
set -e
/build/qemu-9.1.0/build/qemu-system-arm -M mr80x -nographic -monitor none \
  -serial null -nic none -kernel /fw/appsbl.unpadded.elf -s &
QEMU_PID=$!
sleep 2

timeout 25 gdb-multiarch -batch \
  -ex "target remote localhost:1234" \
  -ex "break *0x4a936fa0" \
  -ex "commands" \
  -ex "silent" \
  -ex "printf \"malloc(%u) lr=%x\n\", \$r0, \$lr" \
  -ex "continue" \
  -ex "end" \
  -ex "continue" \
  /fw/appsbl.unpadded.elf 2>&1

kill -9 $QEMU_PID 2>/dev/null || true
wait $QEMU_PID 2>/dev/null || true
