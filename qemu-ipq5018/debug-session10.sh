#!/bin/bash
set -e
rm -f /tmp/timestamps.txt
/build/qemu-9.1.0/build/qemu-system-arm -M mr80x -nographic -monitor none \
  -serial null -nic none -kernel /fw/appsbl.unpadded.elf -s -S &
QEMU_PID=$!
sleep 2

timeout 25 gdb-multiarch -batch \
  -ex "target remote localhost:1234" \
  -ex "break *0x4a926cf4" \
  -ex "commands" \
  -ex "shell date +main_loop:%s.%N >> /tmp/timestamps.txt" \
  -ex "continue" \
  -ex "end" \
  -ex "break *0x4a922d68" \
  -ex "commands" \
  -ex "shell date +bootipq:%s.%N >> /tmp/timestamps.txt" \
  -ex "continue" \
  -ex "end" \
  -ex "continue" \
  /fw/appsbl.unpadded.elf 2>&1 | tail -10

echo "=== timestamps ==="
cat /tmp/timestamps.txt

kill -9 $QEMU_PID 2>/dev/null || true
wait $QEMU_PID 2>/dev/null || true
