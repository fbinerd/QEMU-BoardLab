#!/bin/bash
set -e
/build/qemu-9.1.0/build/qemu-system-arm -M mr80x -nographic -monitor none \
  -serial null -nic "user,id=net0,model=mr80x-gmac,net=192.168.0.0/24,host=192.168.0.2,hostfwd=tcp::80-192.168.0.1:80" \
  -kernel /fw/appsbl.unpadded.elf -s &
QEMU_PID=$!
sleep 10
(sleep 3; curl -sS -m 3 http://127.0.0.1:80/ -o /dev/null 2>/dev/null; curl -sS -m 3 http://127.0.0.1:80/ -o /dev/null 2>/dev/null) &

gdb-multiarch -batch \
  -ex "target remote localhost:1234" \
  -ex "break *0x4a97ee94" \
  -ex "continue" \
  -ex "printf \"=== uip_arp_arpin hit: pc=%x lr=%x ===\n\", \$pc, \$lr" \
  -ex "x/4xb 0x4a9edd64" \
  -ex "printf \"uip_hostaddr bytes above (should be c0 a8 00 01 if set to 192.168.0.1)\n\"" \
  /fw/appsbl.unpadded.elf 2>&1

kill -9 $QEMU_PID 2>/dev/null || true
wait $QEMU_PID 2>/dev/null || true
