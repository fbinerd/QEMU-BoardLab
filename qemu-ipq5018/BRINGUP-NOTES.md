# IPQ5018 / MR80X v5 QEMU board bring-up notes

Goal: a custom QEMU machine (`hw/arm/mr80x.c` in a source-built QEMU, since
no upstream model exists for IPQ5018) that boots the REAL `appsbl.bin` from
the sibling `appsbl` project far enough to get an interactive console over
emulated UART and working Ethernet, plus a working QPIC-NAND model backing
a real flash image file - not a from-scratch reimplementation of appsbl,
the actual vendor-derived binary. Scope explicitly excludes matching real
hardware behavior anywhere it isn't needed for that: no PCI, no USB, no
Bluetooth, no switch (RTL8367) chip.

All findings below are from reading `appsbl`'s own vendor source
(`vendor/u-boot-2016/`) and disassembling its own build output
(`out/u-boot.elf`), not guesswork - re-derive from there if anything looks
wrong, don't trust this file blindly once the vendor source changes.

## 1. CPU / boot entry - confirmed needs ~nothing beyond QEMU's own CPU model

- Real chip: ARM Cortex-A53, but this build is pure AArch32
  (`CONFIG_SYS_CPU="armv7"`, `CONFIG_CPU_V7=y` in `build/u-boot-2016/.config`;
  confirmed `out/u-boot.elf` linked from `arch/arm/cpu/armv7/start.o`, NOT
  the `armv8/` AArch64 entry that also exists in the source tree but isn't
  compiled for this board).
- Disassembled the real `reset`/`save_boot_params_ret` sequence
  (`arm-openwrt-linux-muslgnueabi-objdump -d out/u-boot.elf`, symbols at
  `0x4a920300`-`0x4a920340`): CPSR mode switch to SVC32, SCTLR V-bit clear,
  VBAR set to `_start` - all CP15 coprocessor state, zero memory-mapped
  peripheral access. Confirmed `cpu_init_crit` is **not called** in the
  actual linked binary (jumps straight from the VBAR `mcr` to `bl _main`
  at `0x4a921d30`) - DDR/PLL are already brought up by an earlier boot
  stage (SBL) before `appsbl.bin` ever runs; this build does not redo it.
- No board-specific `arch_cpu_init`/`board_early_init_f` override in
  `board/qca/arm/ipq5018/ipq5018.c` - the generic/weak no-op versions run.
- **Implication**: QEMU's stock ARM CPU model (`-cpu cortex-a53` if the
  arm-softmmu build supports it in AArch32 mode, else a close substitute
  like `cortex-a15`) needs no custom work at all for this stage. Just load
  `appsbl.bin`/`appsbl.unpadded.elf` at `CONFIG_SYS_TEXT_BASE = 0x4A920000`,
  set PC there, done.

## 2. Memory map

- `CONFIG_SYS_SDRAM_BASE = 0x40000000`.
- `CONFIG_SYS_TEXT_BASE = 0x4A920000` (~169 MiB into DRAM).
- Real RAM size not yet pinned down exactly from source; plan to start
  with something safely large (e.g. 512 MiB) mapped as plain RAM at
  `0x40000000` and shrink later only if something depends on the exact
  reported size.

## 3. UART - MSM UART DM (`drivers/serial/qca_uart.c` +
   `arch/arm/include/asm/arch-qca-common/uart.h`)

- Base address used by the real board code: `0x78b0000` (from
  `board/qca/arm/ipq5018/ipq5018.c:1916`,
  `parse_fdt_fixup("/soc/serial@78b0000/...")`). NOTE: the *separate*
  `ipq5018-emulation.dts` (Qualcomm's own internal bring-up profile, not a
  QEMU model, not necessarily what real MR80X v5 hardware uses) has the
  console at `0x78AF000` instead - close but different register block;
  don't mix the two up. Use `0x78b0000`, it's the one the actual C driver
  code references.
- Register layout (offsets from base; two `#ifdef`-gated variants exist in
  `uart.h`, need to confirm at prepare-time which one this board's config
  actually selects before finalizing - both captured here so either is a
  quick constant change):
  - `MR1` = base+0x00, `MR2` = base+0x04
  - Variant A (older?): `CSR`=+0xA0, `TF(x)`=+0x100+4x, `CR`=+0xA8,
    `IMR`=+0xB0, `IRDA`=+0xB8
  - Variant B (looks like what's actually used based on offset density):
    `CSR`=+0x08, `TF(x)`=+0x70+4x, `CR`=+0x10, `IMR`=+0x14, `IRDA`=+0x38
  - `IPR`=+0x18, `TFWR`=+0x1C, `RFWR`=+0x20, `HCR`=+0x24, `DMRX`=+0x34,
    `DMEN`=+0x3C, `NO_CHARS_FOR_TX`=+0x40, `BADR`=+0x44
  - `SR` (status register, not yet located exact offset - grep
    `MSM_BOOT_UART_DM_SR` in the same header before implementing) has bits
    `TXEMT`, `TXRDY` polled by the driver; `MISR` has stale-event bits.
- Driver behavior to replicate (from `qca_uart.c`): init writes MR1=0,
  MR2=8N1 mode, IMR=enabled-mask, TFWR/RFWR watermark values, then a CR
  sequence to reset RX/TX/err-stat and enable TX+RX. `putc` polls
  `SR.TXRDY` before writing a word to `TF`. `getc`/`tstc` poll `SR`/`ISR`
  RX-ready bits, read from `RF` (receive fifo, offset not yet located -
  check header for `MSM_BOOT_UART_DM_RF`).
- **Minimum viable model**: on TF write, emit the byte to QEMU's chardev
  backend (stdio/pty) immediately; SR always reports TXRDY/TXEMT set
  (never backpressure - fine, this is a boot console, not a throughput
  test); CR/MR/IMR/TFWR/RFWR writes just latch into ignored state (no
  behavior needed unless something reads them back and branches on it -
  check for that pattern before assuming it's safe to ignore reads).
  RX: buffer injected chardev input, surface via SR RXLEV +
  RF register for interactive console input over the emulated TTL.

## 4. Clock / mux for UART (`board/qca/arm/ipq5018/ipq5018.c`)

- `uart1_configure_mux()` (line 137), `uart1_set_rate_mnd()` (159),
  `uart1_toggle_clock()` (172), `uart1_clock_config()` (201) - all called
  from `qca_serial_init()` (211), which is what `ipq_serial_init()` calls
  before touching the UART DM registers themselves. These write to the
  GCC (Global Clock Controller) MMIO region - exact base/offsets not yet
  extracted from source; next step before UART will actually produce
  output (if these loop-poll a status bit waiting for a clock-enabled ack
  that never comes on unimplemented MMIO, boot will hang here even with a
  perfect UART DM model downstream).
- **Plan**: implement a generic "clock controller stub" region covering
  the GCC address range that (a) accepts all writes silently, (b) for any
  read, returns whatever was last written OR'd with any "enabled/locked"
  bit the polling loops are checking - determine the exact bits by reading
  `uart1_clock_config()`'s source once this phase starts, not guessed here.

## 5. QPIC NAND (`drivers/mtd/nand/qpic_nand.c`, board hook
   `board_nand_init()` at `ipq5018.c:774`, clock helper
   `qpic_set_clk_rate()` at `ipq5018.c:717`)

- Confirmed via `build/u-boot-2016/.config`: `CONFIG_QPIC_SERIAL=y` (SPI
  mode of the QPIC controller - matches the real "SPI-NAND" flash
  reported for this hardware), `CONFIG_NAND_FLASH=y`,
  `CONFIG_UBI_WRITE=y`. The generic `CONFIG_CMD_NAND`/`CONFIG_CMD_SF` text
  in the defconfig file is misleading dead text - the REAL linked binary
  (`nm out/u-boot.elf | grep _u_boot_list_2_cmd_2_`) has `nand`, `sf`,
  `mtdparts`, `flash`, `ubi`, `erase`, `protect` all actually registered;
  trust the linked symbol list over defconfig text, always (learned this
  the hard way earlier this session - see appsbl's CLEAN_ROOM_STATUS.md).
- `qpic_nand.c` has no `U_BOOT_CMD` of its own - it's a pure MTD-layer
  chip driver; the `nand`/`mtdparts`/`ubi` commands come from generic
  u-boot command code calling into it through the MTD API
  (`mtd->_read`/`_write`/`_erase`).
- Register map not yet extracted - next step. Given we have full driver
  source, this is a transcription task (find the `writel`/`readl` calls
  and their base+offset macros), not blind reverse engineering.
- **Backing store**: model should read/write a flat file on the QEMU host
  representing the whole flash (or at minimum the appsbl+env+rootfs
  partitions we care about), so `out/appsbl-dual-key.bin` and
  `keys/private/*.pem`-signed test images (via
  `openwrt`'s `tplink-cloud-sign.py`) can be written into a virtual flash
  image and read back through the SAME code path a real device would use
  - that's the actual point of building this.

## 6. Ethernet (`drivers/net/designware.c`)

- Synopsys DesignWare MAC (DWMAC) - a very widely used, standard IP block.
  QEMU may already have a compatible model (`hw/net/dwc_eth_qos.c` exists
  upstream for a related-but-not-identical DWC variant used on some other
  SoCs; needs checking for register-level compatibility with what this
  driver expects, not assumed compatible just because both say
  "designware"). `board_eth_init()` at `ipq5018.c:1215` - not yet read in
  detail.
- Base addresses seen in `ipq5018-emulation.dts` (Qualcomm's own, not
  necessarily what the real board uses, but likely close since these are
  SoC-fixed, not board-specific): `gmac1` at `0x39C00000`, `gmac2` at
  `0x39D00000`. Cross-check against the real board's own DTS
  (`ipq5018-mp03.*`/`ipq5018-db-mp03.*` - haven't identified which exact
  file this specific MR80X v5 config selects yet; boot is combined-DTB
  with runtime `machid` selection, not a single compile-time file) before
  trusting these for anything but a starting guess.

## 7. Which DTS/machid to target

- This build compiles ALL `arch/arm/dts/ipq5018-*.dts` variants into one
  combined blob (`dtb_combined.bin`) and the running SBL/appsbl selects
  one by `machid` read from hardware at runtime - there is no single
  "the" DTS file for MR80X v5 identifiable purely from the u-boot source
  tree; it's determined by a machid value baked into the real device
  (SMEM or similar), not found yet.
- **Decision for the emulator (deliberate simplification, not an attempt
  at hardware fidelity)**: rather than hunt down MR80X v5's exact real
  machid/DTS variant among ~18 near-identical `ipq5018-mp03.*-cN.dts`
  files, target `ipq5018-emulation.dts`'s profile
  (`machid = 0x0F040000`) - it's Qualcomm's own reduced/bring-up-friendly
  hardware profile (simpler pin mux, already excludes things we don't
  care about like the RTL8367 switch), and is good enough for what we
  actually need (console + Ethernet + NAND), not a specific real board's
  full peripheral set. Document this choice inline in the QEMU board file
  when it's written, so it's not mistaken for real-hardware-accurate
  later.

## Status / next steps (in order)

1. [done] Boot-entry and memory-map research (this document).
2. [in progress] Finish extracting exact register maps: UART `SR`/`RF`
   offsets, GCC clock controller addresses for `uart1_clock_config()`,
   QPIC NAND register map, DesignWare base address for the real (not
   emulation-DTS) board.
3. Stand up the QEMU source build (Dockerfile, vendored/pinned QEMU
   9.1.0 source, new `hw/arm/mr80x.c` skeleton wired into
   `hw/arm/Kconfig` + `hw/arm/meson.build`) with just CPU+RAM+a
   log-everything catch-all MMIO stub device covering the regions we
   haven't modeled yet, to see exactly where boot actually gets stuck
   against ground truth rather than more static analysis.
4. Implement UART properly, confirm real console text appears.
5. Implement clock stub for UART's polling loops.
6. Implement QPIC NAND backed by a file.
7. Implement/wire Ethernet.
8. Iterate against `out/appsbl.bin` (known-good, byte-identical to real
   hardware) first, then `out/appsbl-custom.bin`/`appsbl-dual-key.bin`,
   then finally test a `tplink-cloud-sign.py`-signed image end-to-end.
