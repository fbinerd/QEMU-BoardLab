#!/bin/bash
set -e
/build/qemu-9.1.0/build/qemu-system-arm -M mr80x -nographic -monitor none \
  -serial null -nic none -kernel /fw/appsbl.unpadded.elf -s &
QEMU_PID=$!
sleep 15

gdb-multiarch -batch \
  -ex "target remote localhost:1234" \
  -ex "interrupt" \
  -ex "printf \"=== stuck at: pc=%x lr=%x sp=%x r0=%x r1=%x r2=%x r3=%x ===\n\", \$pc, \$lr, \$sp, \$r0, \$r1, \$r2, \$r3" \
  -ex "x/8i \$pc" \
  -ex "bt" \
  /fw/appsbl.unpadded.elf 2>&1

kill -9 $QEMU_PID 2>/dev/null || true
wait $QEMU_PID 2>/dev/null || true
