# IM7 reset-button TFTP recovery

This directory contains the reproducible source-side changes used to turn the
known IPC-S21F/IM7 U-Boot into a reset-button recovery loader. Generated
binaries live under `output/` and are intentionally ignored by Git.

## Boot policy

- GPIO 23 high at power-on: the original OEM verification and SPI kernel boot
  path are preserved.
- GPIO 23 low at power-on: U-Boot sets `ipaddr=192.168.2.108`, sets
  `serverip=192.168.2.10`, downloads `im7-recovery-initramfs.uImage`, and runs
  `bootm 0xA1000000`.
- The unsigned recovery-kernel verifier bypass is guarded by a private RAM
  flag that is set only inside the reset-button path. Normal boot executes the
  displaced first instruction of the OEM verifier and continues through it.

The original U-Boot already initializes hardware UART0 and uses it for all
three standard streams (`In`, `Out`, and `Err` report `serial`). Its banner and
board initialization are visible at 115200 baud on the physical UART. The
patch does not replace or disable that initialization; recovery kernel
arguments also retain `console=ttyS0,115200`.

## Generated full SPI image

The completed 8 MiB image is:

```text
output/miboim7-spi-8mb-reset-tftp-192.168.2.10.bin
```

Known SHA-256 values:

```text
OEM SPI:           5ba8b90f01175b9344bd6fef199af3fb73e1b59d5f6bd6ebc1903a3ebfc5e295
recovery U-Boot:   76387ae0165bc0ded4b5ad611936172656da2e50ca0d2af67fe39df0e5b253ad
full recovery SPI: 6207bc4b48c254bb4bc36770b2db5ef2d2d5d88b04c66bcd01ceb3b0d56161ca
```

`build-full-spi.py` refuses unknown inputs. It replaces only partition 0,
bytes `0x000000-0x04ffff`; bytes `0x050000-0x7fffff` remain byte-for-byte
identical to the OEM dump.

Rebuild it with:

```sh
python3 recovery/patch-uboot.py \
  /media/dados_2tb/opw/openwrt-build-tools/tools/firmware-lab/work/miboim7-tudo-sobre/particoes/0_U-Boot.bin \
  recovery/output/0_U-Boot-reset-recovery.bin

python3 recovery/build-full-spi.py \
  /media/dados_2tb/opw/openwrt-build-tools/tools/firmware-lab/work/miboim7-tudo-sobre/miboim7-spi-en25qh64-8mb-20260819.bin \
  recovery/output/0_U-Boot-reset-recovery.bin \
  recovery/output/miboim7-spi-8mb-reset-tftp-192.168.2.10.bin
```

## QEMU validation

Released RESET, normal OEM path from the complete image:

```sh
./run.sh --boot-from-spi \
  --spi-image recovery/output/miboim7-spi-8mb-reset-tftp-192.168.2.10.bin
```

RESET held, automatic TFTP path from the same complete image:

```sh
./run.sh --reset-button --boot-from-spi \
  --spi-image recovery/output/miboim7-spi-8mb-reset-tftp-192.168.2.10.bin
```

The second test has reached the GPL Fullhan recovery kernel banner
`Linux version 3.0.8`, proving that the full SPI image's U-Boot detected GPIO
23, transferred the TFTP image, and handed execution to it. Work on the
minimal recovery console continues separately; it does not affect selection
of the normal versus TFTP boot paths.

## Recovery payload provenance and current status

The OEM Linux 4.9.129 image does not contain initramfs unpacking support, so
it cannot be reused as an embedded initramfs kernel. The current experimental
payload uses the public OpenIPC Linux tree, branch `fullhan-fh8852v100`, at
commit `ceda743d59f935e4147958ba40cc329a3f9dbba0`, starting from
`fh8852_defconfig`. Its UART, interrupt controller, timer, GMAC and SPI maps
match the addresses recovered from the IM7 binaries.

For QEMU bring-up the kernel has an embedded archive built from
`initramfs.list`, LZMA zImage compression, devtmpfs, no VT, no alignment trap,
and optional hardware drivers such as SPI/MTD/MMC/USB disabled. `init.c` is a
libc-free ARM PID 1 intended to supply a deterministic recovery console. The
kernel handoff is confirmed, but output from that minimal PID 1 is the open
bring-up item. Consequently the generated full SPI is suitable for testing
the U-Boot/GPIO/TFTP policy; the initramfs console should be completed and
tested on the target before treating it as a production recovery system.

Before writing real flash, keep an externally verified backup of the original
8 MiB dump and use a programmer procedure that can recover from a failed boot.
