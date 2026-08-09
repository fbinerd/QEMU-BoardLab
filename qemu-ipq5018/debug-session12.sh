#!/bin/bash
set -e
/build/qemu-9.1.0/build/qemu-system-arm -M mr80x -nographic -monitor none \
  -serial null -nic none -kernel /fw/appsbl.unpadded.elf -s &
QEMU_PID=$!
sleep 2

timeout 25 gdb-multiarch -batch \
  -ex "target remote localhost:1234" \
  -ex "break *0x4A91F008" \
  -ex "continue" \
  -ex "printf \"=== trampoline hit #1: pc=%x lr=%x r0=%x ===\n\", \$pc, \$lr, \$r0" \
  -ex "continue" \
  -ex "printf \"=== trampoline hit #2: pc=%x lr=%x r0=%x ===\n\", \$pc, \$lr, \$r0" \
  -ex "continue" \
  -ex "printf \"=== trampoline hit #3: pc=%x lr=%x r0=%x ===\n\", \$pc, \$lr, \$r0" \
  /fw/appsbl.unpadded.elf 2>&1 | tail -40

kill -9 $QEMU_PID 2>/dev/null || true
wait $QEMU_PID 2>/dev/null || true
