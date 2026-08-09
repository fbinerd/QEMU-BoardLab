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

- **Correction (2026-08-09, caught before the first build): base address
  is `0x78AF000`, not `0x78b0000`.** `board/qca/arm/ipq5018/ipq5018.c:1916`
  has `parse_fdt_fixup("/soc/serial@78b0000/...")`, which looked like the
  authoritative source at first - but grepping "78b0000" across every
  `.dts`/`.dtsi` in the vendor tree shows it only ever appears in
  `ipq807x-*`/`ipq40xx-*`/`ipq6018-*` files, **never** in any `ipq5018-*`
  one. `78AF000` is what actually appears throughout every IPQ5018 board
  DTS (`mp02.1`, `sod`, `db-mp03.*`, `mp03.*`, `emulation`) AND the shared
  `ipq5018-soc.dtsi` (`serial@78AF000`, line 19) - that fixup line in
  `ipq5018.c` is almost certainly dead code left over from copying the
  IPQ807x board file without fully updating it for IPQ5018. DTS is
  authoritative over one leftover C reference; use `0x78AF000`.
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

## 4b. Full flash partition table (real dump, not guesswork)

`appsbl` is the complete u-boot (2016.01) - this whole project's clean-room
reproduction of it - but it's only ONE partition among many in the real
SPI-NAND flash; everything before it (sbl1, qsee) runs on the real device
before appsbl ever starts and is out of scope for execution here (see
decision below), but their DATA and every other partition's DATA should
still be present in the emulated flash so u-boot sees the same partition
table (`smeminfo`/`mtdparts`) and content a real device would.

Source: a full, real flash dump, already split into per-partition files at
`/media/dados_2tb/opw/openwrt-build-tools/tools/firmware-lab/work/fw_extracted/`.
Confirmed (byte-exact size sum AND SHA-256 of the appsbl slice, which
matches this project's own byte-identical build's hash
`1c8fbfd9...` exactly) that `FULL_FIRMWARE.bin` in that same directory is
simply these 16 partitions concatenated with zero gaps in mtd-number
order - use it directly as the QEMU NAND backing file, no manual
reassembly needed.

| mtd | name | offset | size | purpose |
|---|---|---|---|---|
| 0 | sbl1 | 0x000000 | 0x080000 | Secondary Boot Loader - Qualcomm proprietary, runs first (loaded by the on-chip ROM/PBL), brings up DDR/PMIC/clocks, loads+hands off to qsee/appsbl. Not executed here (see scope decision, section 7b). |
| 1 | mibib | 0x080000 | 0x080000 | Multi-Image Boot Information Block - Qualcomm's own partition table. This is what `smeminfo` actually reads; it's *why* that command works and partitions show up by name instead of raw mtd numbers. |
| 2 | bootconfig | 0x100000 | 0x040000 | Active rootfs slot selection (A/B boot state). |
| 3 | bootconfig1 | 0x140000 | 0x040000 | Redundant copy of bootconfig, for power-loss safety during a slot switch. |
| 4 | qsee | 0x180000 | 0x100000 | TrustZone/Secure Execution Environment, AArch64 ELF. Not executed here. |
| 5 | devcfg | 0x280000 | 0x040000 | Device/peripheral protection config consumed by TrustZone. |
| 6 | cdt | 0x2C0000 | 0x040000 | Customer/Chip Data Table - board-specific hardware calibration (RF chains, GPIO, PMIC rails), read very early in boot. |
| 7 | appsblenv | 0x300000 | 0x080000 | u-boot environment variables (`bootcmd`, `ipaddr`, `tp_boot_idx`, etc - the ones fixed by hand earlier this project when a prior sysupgrade corrupted them). |
| 8 | appsbl | 0x380000 | 0x140000 | The bootloader itself - what this project reproduces from GPL source. Confirmed byte-identical (SHA-256) to `appsbl/out/appsbl.bin`. |
| 9 | art | 0x4C0000 | 0x100000 | Antenna Reference Table - WiFi radio calibration data and the device's real MAC addresses. |
| 10 | training | 0x5C0000 | 0x080000 | Cached DDR PHY training/timing results from a previous boot, reused by sbl1 to skip a full retrain. |
| 11 | rootfs | 0x640000 | 0x2A00000 | Primary OS partition (UBI). Matches the offset already documented independently in `openwrt-build-tools`'s `recovery_mr80x_v5.md` note - cross-check passed. |
| 12 | rootfs_1 | 0x3040000 | 0x2A00000 | Secondary/alternate OS partition (UBI), same cross-check. |
| 13 | tp-data | 0x5A40000 | 0x840000 | TP-Link/Mercusys vendor persistent data (UBI). |
| 14 | radio | 0x6280000 | 0x440000 | Additional per-radio board/calibration data (UBI), separate from ART. |
| 15 | data | 0x66C0000 | 0x080000 | General persistent config/data partition (UBI) - OpenWrt `rootfs_data`-equivalent. |

Total: `0x6740000` (108,265,472 bytes), matches `FULL_FIRMWARE.bin` exactly.

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
- **Backing store, decided**: use
  `openwrt-build-tools/tools/firmware-lab/work/fw_extracted/FULL_FIRMWARE.bin`
  directly (confirmed byte-exact concatenation of all 16 real partitions,
  see section 4b) as the QEMU NAND model's backing file - `cp` a working
  copy per test run so writes never touch the source dump. This means
  `smeminfo`/`mtdparts` inside the emulator see the SAME real partition
  table, ART MAC addresses, CDT, etc. a real device has, not synthetic
  placeholders. To test a new appsbl build, patch just the appsbl slice
  (offset `0x380000`, length `0x140000` - see table in 4b) of a working
  copy of this file before boot, e.g. with `out/appsbl-dual-key.bin` or a
  `tplink-cloud-sign.py`-signed test image dropped into the rootfs slice.

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

## 7b. Scope decision, confirmed with user: sbl1/qsee are data-only, not executed

`sbl1` (Qualcomm proprietary, undocumented header magic `d1dc4b84...`, not
a plain ELF) and `qsee` (TrustZone, AArch64 ELF) are real binaries we have
byte-for-byte from the flash dump, but reverse-engineering them well
enough to actually *execute* inside QEMU - proprietary DDR/PMIC
sequencing, secure boot crypto, AArch64-to-AArch32 handoff - is an
open-ended undertaking with no public documentation to check against,
unlike `appsbl` where we have full GPL source. Decided (2026-08-09): QEMU
starts CPU execution directly at `appsbl`'s entry (`0x4A920000`), the same
way this project already does on real hardware via `bootelf`/`go` from a
u-boot console - not re-simulating sbl1/qsee's own execution. Their DATA
is still present in the backing flash image (section 4b) so anything
`appsbl` itself reads from those partitions (mibib partition table via
`smeminfo`, ART MAC/calibration, etc.) resolves to real values.

## 8. MILESTONE (2026-08-09): real appsbl.bin boots to an interactive console

First working end-to-end boot, against the real, unmodified
`appsbl/out/appsbl.unpadded.elf` (not a test stub):

```
U-Boot 2016.01 (Nov 11 2024 - 20:42:13 +0800)
DRAM:  256 MiB
...
machid: f040000
...
key 14 addr 0x0100e004 val 0x0
FW GPIO is pressed. Enter firmware recovery mode!
...
Start web server.
...
IPQ5018#
```

The `U-Boot 2016.01 (Nov 11 2024 - 20:42:13 +0800)` banner is
byte-for-byte the same string already documented from the real hardware
in `openwrt-build-tools`'s `recovery_mr80x_v5.md`. `machid: f040000`
confirms the SMEM fake-out (section 7c below) is working exactly as
designed. Unexpectedly, since GPIO reads currently fall through to the
TLMM catch-all stub (always returns 0) and this device treats
GPIO14=0 as "reset button held", it **auto-entered firmware recovery
mode and started the HTTP recovery server** - the exact subsystem this
whole project has spent months on - which was not the target for this
milestone but means the emulator is already deep enough to potentially
test HTTP recovery uploads (including `tplink-cloud-sign.py`-signed
images) without real hardware, once Ethernet is modeled. Ethernet
itself isn't wired yet, so the recovery server's own link-state polling
loop fails and it falls through back to the normal boot flow, still
reaching the `IPQ5018#` shell prompt either way.

### What got fixed to reach this point, in order

1. UART base address corrected (0x78AF000, not 0x78b0000 - section 3).
2. GCC clock stub for `uart1_trigger_update()`'s polling loop (section 4).
3. **SMEM machid fake-out** (new, section 7c below) - without it,
   `fdtdec_setup()` → `smem_get_board_platform_type()` reads
   uninitialized RAM (since we don't execute sbl1, which would normally
   populate SMEM) and `parse_combined_fdt()` calls `hang()` when no DTB
   entry in the combined blob matches. Fixed by writing a single valid
   `smem_alloc_info` entry (type `SMEM_MACHID_INFO_LOCATION`=425, the
   *older* non-partition-table SMEM layout - confirmed
   `CONFIG_SMEM_VERSION_C` is unset for this board, so the simple
   `struct smem { proc_comm[4]; version_info[32]; heap_info;
   alloc_info[506]; }` applies directly at `CONFIG_QCA_SMEM_BASE`
   (`0x4AB00000`), no partition-table parsing needed) pointing at an
   8-byte `{format, machid}` struct with `machid = 0x0F040000`
   (`ipq5018-emulation.dts`'s machid, per the section 7 decision).
4. **Generic timer counter-view registers** (new, section 7d below) -
   `read_counter()`/`__udelay()` polls `gcnt_cntcv_lo`/`gcnt_cntcv_hi`
   at `0x4A2000`/`0x4A2004` (from the `/timer` DT node in
   `ipq5018-soc.dtsi`); without a model, the poll loop spun forever on
   an always-zero "elapsed time". Modeled as a free-running counter that
   jumps forward by a large step on every read of the LO half - not
   wall-clock accurate, but delay loops complete essentially instantly,
   which is what we want for fast iteration anyway.

## 7c. SMEM MMIO region (board/qca/arm/ipq5018/, arch/arm/cpu/armv7/qca/common/smem.c)

`MR80X_SMEM_BASE = 0x4AB00000` (`CONFIG_QCA_SMEM_BASE`). Struct layout
(all fields plain `unsigned`, naturally 4-byte aligned, no padding):

```
struct smem {
    struct smem_proc_comm proc_comm[4];   // 4 * 16 bytes, offset 0x000
    unsigned version_info[32];             // 32 * 4 bytes, offset 0x040
    struct smem_heap_info heap_info;       // 16 bytes,     offset 0x0C0
    struct smem_alloc_info alloc_info[506];// 506*16 bytes, offset 0x0D0
};
struct smem_alloc_info { unsigned allocated, offset, size, reserved; }; // 16 bytes
```

Only `alloc_info[425]` (`SMEM_MACHID_INFO_LOCATION`, from
`board/qca/arm/ipq5018/ipq5018.h`'s `smem_mem_type_t`) needs to be
valid for `smem_get_board_platform_type()`'s first lookup path to
succeed and return immediately - its `format` field is never checked,
only `machid`. Byte offset of `alloc_info[425]` = `0xD0 + 425*16` =
`0x1B60`. Currently written directly into guest RAM in `mr80x_init()`
via `cpu_physical_memory_write()` (not a real MMIO device - SMEM is
just plain shared RAM on real hardware too, no register semantics to
model, just needs the right bytes present before boot).

Every OTHER `smem_read_alloc_entry()` call in the boot log
(`SMEM_BOOT_FLASH_TYPE`, `SMEM_BOOT_FLASH_INDEX`, etc. - the repeated
"smem: read ... failed" lines) fails gracefully with a printed warning
and a hardcoded fallback default - none of them block boot, so none
needed faking (yet - revisit if a later feature depends on one
resolving to a real value instead of its default).

## 7d. Generic timer counter-view registers

`MR80X_TIMER_BASE = 0x4A2000..0x4A2007` (`gcnt_cntcv_lo`/`gcnt_cntcv_hi`
from the `/timer` node in `ipq5018-soc.dtsi`, lines 30-31). Modeled as
a free-running 64-bit counter, LO half advances by `0x100000` on every
read (arbitrary - large enough that any real delay-until-elapsed loop
in `read_counter()`/`__udelay()`/`get_timer()` finishes in a handful of
reads instead of spinning). Not tied to `QEMU_CLOCK_VIRTUAL` or real
wall-clock time - intentional, since fast/deterministic boot matters
more here than timing accuracy for delays that are typically
microsecond-scale on real hardware anyway.

## 9. MILESTONE (2026-08-09): Ethernet link-up + past the MMU/alignment wall

Continuing from section 8: with the MDIO/GEPHY PHY-ID model in place
(section 10 below), `eth0` reports `up Speed :100 Full duplex` - real
progress through `board_eth_init()` - but then hit a genuine ARM data
abort inside `lib/uip/dns.c`'s `dns_init()`.

Root-caused with `gdb-multiarch` attached to QEMU's `-s -S` gdbstub (now
baked into the Dockerfile) rather than continuing log archaeology - this
turned out to be architecturally subtle and not something log-reading
alone would have resolved:

- `DFAR` (Data Fault Address Register) at the fault was `0x4a9ed603`,
  and `DFSR=0x801` decodes to Fault Status = 1 = **Alignment fault**.
- `struct uip_udp_conn` is 9 bytes (4+2+2+1, no padding) - `uip_udp_conns[1]`
  (the DNS resolver's connection, since slot 0 is always taken by
  `dhcpd_init()`'s own socket, called first) lands at an address ending
  in `...5ff`, and its `rport` field 4 bytes in lands on an *odd*
  address - `strh` (halfword store) to an odd address.
- This is **not a QEMU CPU-model bug** - tried both `cortex-a15` and
  `cortex-a7`, identical fault. It's correct ARMv7-A VMSA architectural
  behavior: `target/arm/tcg/hflags.c`'s `aprofile_require_alignment()`
  enforces alignment unconditionally whenever the MMU is disabled
  (`SCTLR.M=0`), because translation-disabled memory defaults to Device
  type architecturally, regardless of `SCTLR.A`.
- The MMU was disabled because `board_init.c`'s `enable_caches()` only
  calls `dcache_enable()` (which sets up an identity-mapped MMU, a
  VMSA prerequisite for treating RAM as cacheable Normal memory) when
  `smem_get_boot_flash()` reports a nonzero flash type - the vendor's
  own comment there says **"Skips dcache_enable during JTAG recovery"**,
  i.e. a real, intentional degraded-boot mode for exactly our situation
  (no valid SMEM flash info), not a bug in appsbl.
- Fixed by faking `SMEM_BOOT_FLASH_TYPE`/`_INDEX`/`_CHIP_SELECT`/
  `_BLOCK_SIZE`/`_DENSITY` (types 498-502) as NAND, matching the real
  device.

That fix unlocked a *different* code path in `board/qca/arm/common/board_init.c`'s
`board_init()`: a nonzero flash type routes through `smem_ptable_init()`
(reads `SMEM_AARM_PARTITION_TABLE`=9, validates a `$TOC`-style magic) -
previously bypassed entirely by the `SMEM_BOOT_NO_FLASH` case, now
mandatory and its failure is **fatal** ("cdp: SMEM init failed", aborts
the whole `initcall_sequence`). Faked a minimal valid `struct smem_ptable`
(correct magic, version=1). That in turn revealed `board_init()` also
hard-requires resolving a `"0:APPSBLENV"` partition via `smem_getpart()`
("cdp: get environment part failed" otherwise) - added one real
`smem_ptn` entry matching the actual flash dump's appsblenv location
(offset `0x300000`, size `0x80000` - see section 4b's partition table).

After all three fixes: **no more data abort, no more fatal SMEM errors** -
boot proceeds straight into real DesignWare-style DMA descriptor setup
for GMAC1 (writes to offsets `0x100c`/`0x1010` - TX/RX descriptor ring
base addresses - and `0x1018`/`0x0`/`0x4`/`0x18`, a plausible
MAC-config + DMA-control register layout). This is genuine forward
progress into the territory this emulator actually needs to model next
(section 10's remaining item: real packet TX/RX, not just link
detection) - not another blocker to route around.

Also noted in passing: `"No ART partition found"` prints and boot
continues (uses the default MAC `00:11:22:33:44:55`) - confirms ART
lookup failure is one of the *soft*-fail SMEM paths, unlike
`APPSBLENV`, consistent with everything else observed about which SMEM
reads are fatal vs. gracefully defaulted.

## 10. MDIO controller + GEPHY internal PHY (the actual Ethernet gate)

`board_eth_init()` doesn't use `drivers/net/designware.c` at all -
IPQ5018 has its own `drivers/net/ipq5018/ipq5018_gmac.c`, which
*additionally* `#include`s the RTL8367 switch driver headers directly.
Real boards configure `s17c_switch_enable` in their DTB's `gmac_cfg`
node for the switch-connected port; `ipq5018-emulation.dts` (our
target profile, section 7) configures neither a switch nor a
`phy_type` for either GMAC - Qualcomm's own bring-up environment
apparently has no real PHY/switch attached either.

The driver doesn't trust a DTB `phy_type` value directly - it reads
the PHY's `MII_PHYSID1`/`MII_PHYSID2` (regnum 2/3) over MDIO and
switches on the *result* (`ipq5018_gmac.c` around line 1024). Returning
the internal GEPHY's ID (`0x004DD0C0`, from `arch-ipq5018/ipq5018_gmac.h`)
for the `phy_address` `gmac1_cfg` uses (7, consistent across every DTB
checked) makes it take the simple internal-PHY path
(`ipq_gephy_phy_init()` in `drivers/net/ipq_common/ipq_gephy.c`)
instead of needing the RTL8367 switch chip modeled - deliberately out
of scope (section 6). GMAC2/`gmac2_cfg` (`phy_address` 1) gets `0xffff`
(standard "nothing answered") for any register and is left to fail the
same "not mapped" way it already did.

MDIO controller (`drivers/net/ipq5018/ipq5018_mdio.c`/`.h`) - base
`0x88000` (this is what the very first boot log's "Invalid read/write
at addr 0x88040/0x88044/0x88050" actually was). Registers:
`CTRL_0`=+0x40, `CTRL_1`=+0x44 (holds `mii_id<<8 | regnum` from the
last write), `CTRL_2`=+0x48, `CTRL_3`=+0x4c (read result), `CTRL_4`=+0x50
(command + busy bit `1<<16`, cleared instantly in the model - same
"exits the poll loop on first read" trick as the GCC `CMD_RCGR`
registers). Modeled: writes to `CTRL_1` latch the `(mii_id, regnum)`
pair; a `CTRL_4` write with the START bit computes and latches the
response into `CTRL_3` immediately.

`ipq_gephy_phy_init()`'s three PHY ops (`get_link_status`/`get_speed`/
`get_duplex`) all read a *single* vendor register,
`GEPHY_PHY_SPEC_STATUS` (regnum 17, `drivers/net/ipq_common/ipq_gephy.h`),
which packs link/speed/duplex into one word. Returning
`GEPHY_STATUS_LINK_PASS(0x400) | FULL_DUPLEX(0x2000) | SPEED_100MBS(0x80)
= 0x2480` for `(phy_address=7, regnum=17)` produced the
`"eth0 up Speed :100 Full duplex"` boot message - real driver logic,
not a bypass.

Along the way, discovered `ipq5018_enable_gephy()`'s clock/reset block
(`GCC_GEPHY_BCR/MISC/RX_CBCR/TX_CBCR` at `0x01856000`+) and the whole
GMAC clock block (`GCC_GMAC_CMD_RCGR` etc. at `0x01868000`+) are
*outside* the original 256KiB `MR80X_GCC_SIZE`. Rather than widen the
region piecemeal for every newly-discovered GCC sub-block (SDCC1, PCIe,
USB, QPIC_IO_MACRO are all in the same address family per
`ipq5018.h`), widened it once to 1MiB and **generalized the `CMD_RCGR`
busy-bit-clear-on-read behavior to every register in the block**
instead of enumerating each of the ~18 `*_CMD_RCGR` addresses by name -
harmless for non-`CMD_RCGR` registers since nothing reads their bit 0
back expecting anything else.

## 11. MILESTONE (2026-08-09): real GMAC1 DMA + a genuine TCP handshake from the host

Implemented `MR80XGmacState` as a proper `SysBusDevice` with a real
`NICState` (wired to whatever `-nic`/`-netdev` the invocation supplies)
instead of the earlier catch-all stub - see the big comment block above
`struct MR80XGmacState` in `mr80x.c` for the full design. Key point:
`ipq_eth_send()`/`ipq_eth_recv()` (`drivers/net/ipq5018/ipq5018_gmac.c`)
drive TX/RX by polling an ownership bit **inside the 32-byte descriptor
struct in guest RAM itself**, not a hardware status register - so the
device model's job on a `DmaTxPollDemand` write is simply: read the
current descriptor, extract `buffer1`/`length`, `qemu_send_packet()`,
clear the ownership bit, advance to the next descriptor via its own
`data1` chain pointer (the ring is a driver-maintained linked list, not
a fixed stride). RX is the mirror, driven by the NIC's `.receive`
callback instead of a register write.

Two connection-plumbing gotchas, both fixed:

- `-netdev user,id=net0` alone left `qemu_configure_nic_device()` unable
  to find a match ("nic mr80x-gmac.0 has no peer") - it searches
  `nd_table`, which only `-nic`/legacy `-net nic` populate, not a bare
  `-netdev`. Use `-nic user,model=mr80x-gmac,...` instead.
- appsbl's recovery `httpd` hardcodes its own IP to `192.168.0.1`
  (`net.c`: `uip_ipaddr(ipaddr, 192,168,0,1); uip_sethostaddr(ipaddr);`)
  rather than obtaining one via DHCP (it *serves* DHCP, doesn't consume
  it) - slirp's default subnet is `10.0.2.0/24` and doesn't route to
  that address on its own. Needs `net=192.168.0.0/24,host=192.168.0.2`
  plus a `hostfwd=tcp::8080-192.168.0.1:80` rule to actually reach it
  from outside.

Result with both fixed: `curl http://localhost:<hostfwd-port>/` from
the **host**, against the real, unmodified `appsbl.unpadded.elf`
running inside QEMU, gets past the TCP handshake (`curl` reports
"Connected") - meaning a real SYN reached the guest through
`mr80x_gmac_receive()`, `uip_input()` processed it, and a real SYN-ACK
came back out through `mr80x_gmac_do_tx()` to the host. The GET
request after that doesn't get an HTTP response yet within a several-
second window - not yet root-caused (candidates: uIP's periodic timer
processing not being driven correctly by the fast/non-realtime timer
model in section 7d, or a bug in `cur_rx_desc`/`cur_tx_desc` chain
advancement past the very first packet - only confirmed one full
round trip so far, not sustained traffic). Next debugging session:
same `gdb-multiarch` approach as section 9, or add temporary
`qemu_log_mask` tracing to `mr80x_gmac_receive`/`do_tx` to see whether
the GET request's packet is even reaching `mr80x_gmac_receive` at all.

## 12. GPIO14/reset-button fix + normal (non-recovery) boot exposes the NAND gap directly

The user asked for the emulator to boot *normally*, like real hardware,
not always land in recovery mode. `check_fw_gpio()`
(`board/qca/arm/common/cmd_bootqca.c`) reads GPIO14 (`CONFIG_RESET_KEY`)
*active low* (`return !(val & 0x1)`) - the generic TLMM catch-all
stub's default 0 read reads as "button held", auto-triggering recovery
on every boot regardless of what was actually being tested. Fixed
with a dedicated TLMM ops handler (`mr80x_tlmm_ops`) that returns
all-ones for every read instead of the generic stub's zero - "button
not pressed", the correct idle state for a device nobody is touching.
Confirmed: `key 14 addr 0x0100e004 val 0xffffffff`, no more
auto-recovery, boot takes the normal path.

That normal path immediately hits a *different*, real crash: a
prefetch abort at `pc=0x0000000c` - the classic "called through a
null function pointer, offset 0xc into some struct/vtable" signature.
`LR` at the fault doesn't resolve to any symbol in `System.map` at
all (it's stack *data* being read back as a return address, meaning
the stack itself is already corrupted by this point, not just a bad
single jump). Root cause not fully confirmed yet but strongly
indicated: the normal boot path tries to actually load a kernel/rootfs
from NAND (unlike the recovery-mode path, which only serves HTTP and
never touches block storage) - our QPIC NAND is still just an unimp
stub ("Qpic controller not support serial NAND" already printed
earlier in the same boot), so any UBI/JFFS2/MTD code that assumes NAND
init succeeded is working with an uninitialized device table.
**Implementing real QPIC NAND (already section 5/item 7 on this list,
backed by `FULL_FIRMWARE.bin`) is very likely the actual fix**, not
another targeted register fake.

While chasing this, also found and fixed a **real, separate** bug:
`reset_cpu()` (`board/qca/arm/ipq5018/ipq5018.c`) -> `qti_scm_pshold()`
tries an SCM/TrustZone call first and only falls back to writing
`GCNT_PSHOLD` (`0x004AB000`) directly if that fails - modeled the
PSHOLD write to trigger a real `qemu_system_reset_request()`, matching
the real hardware's power-cycle-on-write behavior. Confirmed via log
that this fallback write **never actually happens** - `grep -c
GCNT_PSHOLD` on a full crash-loop capture is 0, and only one
`"U-Boot 2016.01"` banner ever appears despite dozens of "Resetting
CPU .../resetting ..." message pairs. This means the `scm_call()`
attempt itself is what's actually going wrong (an SMC instruction with
no EL3/secure-monitor configured in this minimal machine, plausibly
undefined-behavior-ish in TCG without one) - the panic-inside-panic
recursion this produces is what accounts for the repeating,
progressively-lower-address crash pattern seen in raw logs, not a
single hang. The PSHOLD fix is real and correct for the *normal* path
but doesn't get exercised until `scm_call()`'s behavior is also
addressed - worth a `gdb-multiarch` session on `scm_call()` specifically
if crash-recovery-triggered resets matter for some future test (mainline
non-crash reboots, e.g. a `reset` console command, would hit the same
`qti_scm_pshold()` path and are equally unverified yet).

## Status / next steps (in order)

1. [done] Boot-entry and memory-map research.
2. [done] UART, GCC clock stub, SMEM machid fake-out, generic timer -
   real `appsbl.unpadded.elf` reaches the `IPQ5018#` interactive
   console prompt (section 8).
3. [done] QEMU source build (Dockerfile, vendored QEMU 9.1.0,
   `hw/arm/mr80x.c` wired into `hw/arm/Kconfig` + `hw/arm/meson.build`
   under `arm_ss` - NOT `system_ss`, that was the first build error,
   `system_ss` files don't get the `-I` path for `cpu.h`).
4. [done] MDIO + GEPHY PHY-ID/link-status model (section 10) -
   `eth0 up Speed :100 Full duplex`, genuine driver logic, not a bypass.
5. [done] SMEM flash-type + partition-table + `0:APPSBLENV` fakes
   (section 9) - cleared the MMU/alignment wall, no more fatal SMEM
   errors or data aborts. Boot now reaches real GMAC1 DMA descriptor
   setup (writes to `0x100c`/`0x1010`/`0x1018`/`0x0`/`0x4`/`0x18` -
   TX/RX descriptor ring addresses + MAC config/DMA control, a
   DesignWare-ish layout).
6. [done] Real GMAC1 DMA TX/RX + `slirp` networking (section 11) - TCP
   handshake with a real host-side `curl` succeeds against the real
   `appsbl.unpadded.elf`. HTTP-level response not confirmed working
   yet past the handshake - see section 11 for the specific open
   question and how to debug it next.
7. Next: QPIC NAND backed by `FULL_FIRMWARE.bin` (section 4b/5) -
   currently falls through to "Unknown flash type" / "Qpic controller
   not support serial NAND", gracefully non-fatal but means no real
   partition data (ART, rootfs, etc.) is reachable yet - and the fake
   `smem_ptable` (section 9) only has one partition (`APPSBLENV`), not
   the full real layout, so partition lookups other than the env one
   will also come up empty until this is backed by real NAND.
8. GPIO/TLMM currently an unmodeled catch-all stub that happens to
   return 0 for everything, including GPIO14 (reset button) - that's
   why recovery mode auto-triggers on every boot right now. Worth a
   real (if simple) GPIO model once NAND/Ethernet are in, so boot mode
   is deliberately selectable instead of an accident of the stub's
   default return value.
9. Once NAND + Ethernet work: test `out/appsbl-custom.bin` and
   `out/appsbl-dual-key.bin` (not just plain `appsbl.bin`), then
   finally a `tplink-cloud-sign.py`-signed image through the actual
   HTTP recovery upload path - the original point of building this.
