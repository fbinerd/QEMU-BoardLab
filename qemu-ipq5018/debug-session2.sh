#!/bin/bash
set -e
/build/qemu-9.1.0/build/qemu-system-arm -M mr80x -nographic -monitor none \
  -serial null -kernel /fw/appsbl.unpadded.elf -s -S &
QEMU_PID=$!
sleep 1

gdb-multiarch -batch \
  -ex "target remote localhost:1234" \
  -ex "break *0x4a980a48" \
  -ex "continue" \
  -ex "printf \"=== dns_init entry: sp=%x lr=%x r0=%x r1=%x r2=%x ===\n\", \$sp, \$lr, \$r0, \$r1, \$r2" \
  -ex "break *0x4a980a5c" \
  -ex "continue" \
  -ex "printf \"=== before bl uip_udp_new: sp=%x ===\n\", \$sp" \
  -ex "break *0x4a97db34" \
  -ex "continue" \
  -ex "printf \"=== uip_udp_new entry: sp=%x lr=%x r0=%x r1=%x ===\n\", \$sp, \$lr, \$r0, \$r1" \
  -ex "break *0x4a980a60" \
  -ex "continue" \
  -ex "printf \"=== back in dns_init after call: sp=%x r0=%x ===\n\", \$sp, \$r0" \
  -ex "x/8xw \$sp" \
  -ex "stepi 5" \
  -ex "printf \"=== after epilogue steps: sp=%x pc=%x ===\n\", \$sp, \$pc" \
  /fw/appsbl.unpadded.elf

kill $QEMU_PID 2>/dev/null || true
wait $QEMU_PID 2>/dev/null || true
