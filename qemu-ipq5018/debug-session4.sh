#!/bin/bash
set -e
/build/qemu-9.1.0/build/qemu-system-arm -M mr80x -nographic -monitor none \
  -serial null -kernel /fw/appsbl.unpadded.elf -s -S &
QEMU_PID=$!
sleep 1

gdb-multiarch -batch \
  -ex "target remote localhost:1234" \
  -ex "break *0x4a822868" \
  -ex "continue" \
  -ex "printf \"=== at 0x4a822868: pc=%x lr=%x r0=%x r1=%x r2=%x r3=%x ===\n\", \$pc, \$lr, \$r0, \$r1, \$r2, \$r3" \
  -ex "x/12i 0x4a822840" \
  -ex "info registers" \
  /fw/appsbl.unpadded.elf

kill $QEMU_PID 2>/dev/null || true
wait $QEMU_PID 2>/dev/null || true
