# Amlogic GXLX p271 / MXQ Pro QEMU board

This is an isolated QEMU machine for the `gxl_p211_v1` MXQ Pro.  It uses the
same layout as `qemu-ipq5018` and `qemu-fullhan-im7`: its own machine name,
container, board source, run script, and bring-up log.

## Boot boundary

The 4 MiB `bootloader` region at the beginning of `emmc_full.img` is an
Amlogic secure-boot image. BL2 is visible, but BL30/BL31/BL32/BL33 are stored
in a signed/encrypted FIP; there is no runnable factory U-Boot binary in the
dump. Consequently this machine models the real BL31-to-BL33 handoff and
starts an upstream `p212_defconfig` U-Boot at its real link address
`0x01000000`. It does not claim to emulate the proprietary BootROM, DDR
training, or secure-world firmware.

That U-Boot uses the real Meson register interfaces and boots the unmodified
Android `boot` partition from the full eMMC image. The boot command reads
sector `0x2ae000` (16 MiB), supplies the decoded factory
`gxlx_p271_1g` DTB, and invokes `bootm`'s Android image support. Both the DTB
and initramfs are relocated below `0x10000000`, inside the factory ARM32
kernel's low-memory mapping.

## Confirmed image facts

- SoC: GXLX, p271, four Cortex-A53 cores; factory kernel is ARM32.
- RAM: 1 GiB, mapped at physical address zero.
- eMMC image: 7,818,182,656 bytes, 15,269,888 512-byte sectors.
- eMMC controller C: `0xd0074000`, GIC SPI 218.
- AO UART: `0xc81004c0` (`ttyS0`, 115200).
- GICv2: distributor `0xc4301000`, CPU interface `0xc4302000`.
- Boot partition: sector `0x2ae000`, size `0x8000` sectors.

The image has an Amlogic MPT/EPT rather than GPT/MBR. The emulator therefore
exposes the whole image as one eMMC and U-Boot reads the boot partition by
its native raw-sector location. Linux receives the original DT partition
nodes and sees the same complete storage device. `run.sh` always uses a
temporary QEMU snapshot overlay, so guest writes never modify the source
dump.

## Verified boot result

The serial boot reaches Android 9 userspace from the factory image:

- Linux creates `mmcblk0` as an 8 GiB eMMC and imports all 20 Amlogic
  partitions;
- `odm`, `product`, `system`, and `vendor` mount from `mmcblk0p17`, p19, p18,
  and p16 respectively;
- Android init reaches both first and second stage;
- services including `hwservicemanager`, `surfaceflinger`, `zygote`, `media`,
  `netd`, and `wificond` are launched;
- the interactive Android serial shell appears as `console:/ $`.

The guest falls back from eMMC high-speed negotiation and continues at the
legacy clock. `surfaceflinger` restarts because EGL/Mali/display hardware is
not modeled; this does not prevent the serial userspace boot and shell.

## Usage

```sh
./run.sh
./run.sh --stop-autoboot
./run.sh --trace
./run.sh --emmc-image /path/to/emmc_full.img
```

`--trace` logs unmapped/unimplemented accesses and is intended for iterative
bring-up. Normal runs keep that noise off.

## Current modeled surface

- one Cortex-A53 starting in AArch64 at the BL33 handoff;
- 1 GiB RAM;
- GICv2 plus the ARM generic timer PPIs from the factory DT;
- Meson AO UART with interactive RX/TX;
- Meson GX SD/eMMC descriptor interface backed by QEMU's eMMC model;
- broad AO/CBUS/HIU/APB read/write stubs so clock/pinctrl setup can proceed.

Video, audio, USB, Ethernet, Mali, secure monitor services, thermal/ADC and
the three secondary CPUs are not modeled yet. They are not prerequisites
for the serial Android boot path, but vendor drivers may expose the next
required poll/status register during bring-up; use `--trace` for that work.
