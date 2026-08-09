#!/bin/bash
set -e
/build/qemu-9.1.0/build/qemu-system-arm -M mr80x -nographic -monitor none \
  -serial null -kernel /fw/appsbl.unpadded.elf -s -S &
QEMU_PID=$!
sleep 1

gdb-multiarch -batch \
  -ex "target remote localhost:1234" \
  -ex "break *0x4a980a5c" \
  -ex "continue" \
  -ex "printf \"=== before uip_udp_new call ===\n\"" \
  -ex "x/12xb 0x4a9ed5f6" \
  -ex "set {int}0x4a9ed5f6 = 0x12345678" \
  -ex "printf \"write test at base: \"" \
  -ex "x/4xb 0x4a9ed5f6" \
  -ex "set {int}0x4a9ed5ff = 0x12345678" \
  -ex "printf \"write test at base+9: \"" \
  -ex "x/4xb 0x4a9ed5ff" \
  -ex "set {int}0x4a9ed603 = 0xabcdabcd" \
  -ex "printf \"write test at fault addr base+13: \"" \
  -ex "x/4xb 0x4a9ed603" \
  -ex "info mtrr" \
  /fw/appsbl.unpadded.elf

kill $QEMU_PID 2>/dev/null || true
wait $QEMU_PID 2>/dev/null || true
