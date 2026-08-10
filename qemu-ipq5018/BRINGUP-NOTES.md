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

## 13. QPIC NAND: device ID detection via a real BAM cmd-pipe model

Implemented the `NAND_VERSION` gate (`board+0x4F08`, needs
`hw_ver>=2`) so `board_nand_init()` stops bailing out immediately with
"Qpic controller not support serial NAND". Past that gate,
`qpic_nand_fetch_id()`/`qpic_nand_read_reg()` don't touch NAND
registers directly at all - they build a `struct cmd_element` array in
RAM, wrap it in one `struct bam_desc`, write it into the "cmd pipe"'s
descriptor FIFO, then kick the BAM engine by writing the new FIFO
offset to `BAM_P_EVNT_REGn(2,...)` (base `0x07984000`). Modeled a
minimal synchronous BAM: on that kick write, read back the new
descriptor, and when `BAM_DESC_CMD_FLAG` is set, interpret its buffer
as concatenated `cmd_element`s and apply each as a direct read/write
against a shared NAND register file (the same one plain MMIO to
`0x079B0000` uses) - `CE_WRITE_TYPE` writes the register, `CE_READ_TYPE`
writes the register's value out to the RAM address the driver gave
(see `bam_add_cmd_element()`'s "value is a destination pointer for
reads" comment). `addr_n_cmd` only keeps the register address's low 24
bits (top byte repurposed for cmd type) - reconstructable as
`0x07000000 | low24` since every QPIC NAND register lives at
`0x079Bxxxx`. `NAND_EXEC_CMD` write with a `NAND_FLASH_CMD` value of
`NAND_CMD_FETCH_ID` (`0x0B`) synthesizes a fake GigaDevice
`GD5F1GQ4RE9IG` ID (`{0xc8,0xc1}`, chosen because its
page/block/density already match the SMEM flash-type fakes from
section 9). Confirmed: `Serial Nand Device Found With ID : 0xc8 0xc1
... Device Size:128 MiB, Page size:2048 ...`. Only ID/probe works -
real page data (kernel/env/rootfs) still isn't backed by anything, see
next-steps.

## 14. MILESTONE: MVBAR/SMC trampoline - normal boot survives past autoboot

The GPIO14 fix (section 12) exposed a *different* crash after "Hit any
key to stop autoboot", byte-identical (`lr=0x4a82286c`) whether or not
NAND ID detection (section 13) succeeds - proof it was never a NAND
issue. Root cause: `do_bootipq()`'s very first hardware access is
`qca_scm_call(SCM_SVC_FUSE, QFPROM_IS_AUTHENTICATE_CMD, ...)` - a bare
`smc #0` instruction (`arch/arm/cpu/armv7/qca/common/scm.c`). No
secure-world firmware runs in this machine (section 7b), so `MVBAR`
(Monitor mode's vector base) was never programmed by anyone, and `smc`
faulted into nothing.

Fixed by writing a 2-instruction trampoline (`mvn r0,#3 ; movs pc,lr`
- sets r0 to `SCM_EOPNOTSUPP`/-4 and returns) and pointing the CPU's
`env.cp15.mvbar` at it directly from board C code at reset time (guest
code has no legitimate way to set MVBAR itself without secure
firmware). `scm.c`'s own error-remap table turns `SCM_EOPNOTSUPP` into
`-EOPNOTSUPP`, which every caller already treats as "SCM not present,
continue without it" - matching real hardware-without-TrustZone
behavior. `is_scm_armv8()`'s own calling-convention probe also just
reads r0, so one trampoline transparently handles both the legacy and
"armv8_32" SCM call conventions used across this file.

Getting the trampoline's *address* right took three tries, root-caused
via `-d int` exception tracing (`IFSR 0xd` = permission fault):
`arch/arm/lib/cache-cp15.c`'s `dram_bank_mmu_setup()`
(`CONFIG_IPQ_NO_RELOC` path, which `ipq5018.h` enables) marks the
*entire* 4GB address space `SHARED_DEVICE` (execute-unfriendly) first,
then for DRAM specifically marks only the *one* 1MiB section
containing `CONFIG_SYS_TEXT_BASE` (`0x4A920000`) as exec-friendly -
every other MiB of DRAM, even real backing RAM QEMU provides, keeps
the device-like attribute and can't be fetched from. The trampoline
now lives at `appsbl-entry - 0x1000`, guaranteed inside that one safe
section, written *after* the ELF/image load so it can't be clobbered.

Confirmed: normal boot now reaches "Hit any key to stop autoboot",
correctly proceeds (GPIO14 read as not-pressed), and - since no real
kernel data is readable yet - cycles through a *clean* reset instead
of corrupting state, exactly what real hardware would do finding no
valid kernel.

## 15. GMAC1 DMA reset-poll fix + MR80X_RECOVERY - reaching the real recovery HTTP server

Two more fixes were needed to actually reach and test the recovery
HTTP server (the original point of this whole emulator):

**a) `ipq_mac_reset()`'s reset-bit poll never terminated.** It writes
`DMAMAC_SRST` (bit 0) into `dma_reg->busmode`
(`gmac_base+0x1000+0x00`) then busy-polls until it self-clears (real
DMA hardware clears it within a few bus cycles) - our generic
register-latch model just stored whatever was written, so the poll
spun forever, and `ipq_eth_init()` never reached its RX/TX descriptor
ring setup. This is why `eth0 up Speed :100` printed (PHY link check,
*before* the reset call) but no ARP reply for the guest's own IP ever
went out - the RX ring was never programmed for the driver to have
anywhere to receive an incoming packet into. Fixed by clearing bit 0
on readback of `busmode`, same convention already used for the GCC
block's `*_CMD_RCGR` busy bits.

**b) Recovery mode needs a deliberate way in.** Added `MR80X_RECOVERY`
env var (checked once in `mr80x_init()`): when set, GPIO14 reads as
pressed (bit0 clear) instead of the section-12 default of "not
pressed" - i.e. models holding the real reset button, without
regressing the section-12 fix (default boot is still normal boot).

**c) docker's own `-p` port mapping breaks slirp's hostfwd.**
Confirmed via `-object filter-dump` packet capture: connections
arriving through docker's `-p HOST:CONTAINER` mapping have their
source address rewritten to docker's bridge gateway (`172.17.0.1`
typically) before slirp ever sees them. Slirp faithfully reflects that
address into the guest-visible SYN packet; the guest then tries to ARP
for `172.17.0.1` to route its SYN-ACK back, gets no reply (that
address isn't on the emulated LAN subnet at all), and the connection
just times out forever - looks identical to a "the guest can't do TCP"
bug from the outside, but is entirely a docker networking artifact.
Fixed in `run.sh` by using `--network host` instead of `-p`, removing
docker's NAT layer entirely.

**d) The guest's own recovery-mode IP is 192.168.1.1, not
192.168.0.1.** `NetLoopHttpd()` (`net/net.c`) hardcodes
`uip_sethostaddr(192.168.0.1)` - but "`*** Warning - readenv() failed,
using default environment`" (printed every boot, since real NAND page
reads aren't implemented yet - section 13 only does ID detection) means
that hardcoded value gets overridden by whatever the *compiled-in*
default environment's own network setup applies, which turned out to
be `192.168.1.1` (verified by reading `uip_hostaddr`'s raw memory,
`net.c`'s `B uip_hostaddr` symbol, from QEMU C code at packet-receive
time - not by touching vendor source). `run.sh` now defaults
`--guest-ip` to `192.168.1.1` accordingly, with a `--guest-ip` override
in case a future real-NAND-backed environment changes this.

**Confirmed end-to-end**: `./run.sh --recovery appsbl.unpadded.elf`,
then `curl http://localhost:8080/` from the host returns the *real*
recovery-mode "Firmware Upgrade" HTML page served by the genuine,
unmodified `appsbl.unpadded.elf` - ARP resolves, TCP handshake
completes, HTTP GET is answered correctly by uIP's httpd running
inside the emulated CPU.

## 16. MILESTONE: a tplink-cloud-sign.py-signed image passes real RSA verification

The actual point of this entire emulator, confirmed end-to-end:

1. Added a second fake SMEM partition table entry, `"rootfs"` (no
   `"0:"` prefix, matching the real flash dump exactly - see section
   4b's partition #11, offset `0x640000`/size `0x2A00000`) alongside
   the existing `0:APPSBLENV` one. Without it,
   `nm_upgradeFirmware()`'s (`lib/nvrammanager/nm_fwup.c`) call to
   `smeminfo` finds no partition named `QCA_ROOT_FS_PART_NAME`
   (`"rootfs"`), leaving `rootfs_flash_size=0`, and every upload -
   *regardless of signature validity* - was rejected upfront with
   "Bad file size: ... flash: 0" before signature checking was ever
   reached.
2. `./run.sh --recovery out/appsbl-custom.bin` (the clean-room build
   with our own RSA-2048 key swapped in, from earlier work in the
   `appsbl` project).
3. Signed a small test payload:
   `tplink-cloud-sign.py sign --alg 2048 --private-key
   keys/private/firmware-rsa2048-private.pem --payload ... --out
   signed-test.bin` (the key matching what's embedded in
   `appsbl-custom.bin`).
4. `curl -X POST http://localhost:8080/testurl -F
   firmware=@signed-test.bin` (the recovery page's real upload form -
   `enctype="multipart/form-data"`, field name `firmware`, action
   `testurl`) against the running emulator.

**Console output confirms the real, unmodified vendor code path ran
end-to-end**: multipart boundary parsing found the file correctly,
`smeminfo` found the `rootfs` partition and its real size, the
size-vs-flash check passed, then:

```
RSA2048 PSS
Firmware checking passed
```

- the exact `NM_INFO`/verification-success trace from
`rsaVerifyPSSSignByBase64EncodePublicKeyBlob()`
(`lib/nvrammanager/nm_fwup.c`). This is independent, dynamic
confirmation (not just static source reading) that
`tplink-cloud-sign.py`'s from-scratch clean-room RSA-2048/PSS signing
implementation is byte-accurate against the real vendor verification
code, and that the `build-custom` key-swap process produces a
genuinely working bootloader.

The subsequent NAND erase/write does fail ("Attempt to write outside
the flash area", "Writing to NAND... FAILED!") - expected, since real
NAND page read/write still isn't implemented (only ID detection,
section 13) - but this happens *after* signature verification already
passed, so it doesn't affect the validation result. The HTTP response
was still "Upgrade Success" (`nm_upgradeFirmware()`'s return path
apparently doesn't propagate this specific later NAND failure back
into the immediate HTTP response).

## 17. UART line-ending normalization + real NAND page reads (MR80X_NAND_IMAGE)

Two more fixes from live interactive-TTL testing with the user:

**a) Garbled interactive console.** Root-caused by capturing raw bytes
with *zero* terminal/pty involvement (plain shell redirection, then
later confirmed again through a real pty via `script`): the real
appsbl binary's own serial output contains bare `\r` with no `\n` at
all between most lines - a typical boot log had 66 `\r` vs only 2
`\n`, zero `\r\n` pairs. This comes from `qca_uart.c`'s
`msm_boot_uart_dm_write()` path
(`msm_boot_uart_replace_lr_with_cr()`) unconditionally expanding every
`\n` to `\r\n` on top of call sites that already `printf` literal
`\r\n` themselves - genuine vendor behavior, not introduced by this
emulator. An initial attempt to "fix" this by pacing UART TX to
~115200 baud (`g_usleep(87)` per byte) addressed a real but
*secondary* readability issue (TCG blasts the whole boot log through
in milliseconds) but not the actual cause - confirmed still garbled
even with pacing, since pacing only affects timing, not content. The
real fix normalizes at the UART TX emulation layer (not vendor
source): the first `\r` or `\n` of a line ending expands to a proper
`\r\n`; anything immediately following that's also `\r`/`\n` (the
redundant partner, or a repeat) is swallowed. Other control characters
(`\b`, used by the "Hit any key to stop autoboot" countdown to rewrite
a digit in place) are left untouched. Verified byte-for-byte: 27 CR,
29 LF, 27 CRLF pairs, zero bare CR in a full boot log capture.

**b) Real QPIC NAND page reads.** The user correctly diagnosed that
"readenv() failed" (and, they suspected, the second-boot-cycle hang -
see section 18) traced back to only NAND device ID detection ever
being implemented (section 13), never real page *data*. Implemented
`qpic_nand_page_scope_read()`'s actual data path:

- Extended the BAM engine (previously only the cmd pipe, for register
  access) to also drive the **data-producer pipe** (pipe 1, real page
  reads) and **status pipe** (pipe 3, per-codeword auto-status).
- Each codeword's data descriptor copies real bytes from
  `MR80X_NAND_IMAGE` (an env var pointing at a raw full-flash dump,
  e.g. `FULL_FIRMWARE.bin`, mmap'd read-only in `mr80x_init()`) at the
  current page's file offset - tracked via a running byte counter
  reset whenever `NAND_ADDR0` is written (start of a new page-scope
  read) and advanced by each descriptor's length, so the driver's own
  per-codeword destination-pointer math reconstructs the full page
  correctly without this emulator needing to replicate the codeword
  split itself. OOB/spare bytes past the main page size are filled
  `0xFF` (not captured by a raw dump, and nothing that matters for
  booting reads OOB data).
- Status pipe descriptors always report a clean 12-byte record
  (`flash_sts=buffer_sts=erased_cw_sts=0`), matching
  `qpic_nand_check_read_status()`'s success path.
- Generalized the kick (`EVNT_REGn` write) handler to process however
  many descriptors were actually added since the last kick (previously
  assumed exactly one - true for ID fetch, but page-scope-read's last
  codeword batches 2 data descriptors in one kick), and to mask the
  FIFO offset by the pipe's *real* configured size (from
  `BAM_P_FIFO_SIZESn`) instead of a fixed 16-bit wrap. The fixed wrap
  was a genuine bug caught mid-implementation: an early "status
  ffffffff"/-EPERM failure traced to enough kicks accumulating past a
  real (smaller) FIFO's size, so the wraparound math pointed at the
  wrong descriptor and `dev->status_buff` was never actually written.

**Confirmed working against the real dump**: NAND page reads no longer
fail. `readenv()` now gets as far as a CRC check on genuinely-extracted
bytes - "`bad CRC, using default environment`" - and checking the raw
file directly (`xxd -s 0x300000 FULL_FIRMWARE.bin`) shows the
APPSBLENV region is entirely `0xFF` (erased) in this specific captured
dump, so "bad CRC" is *correct*, accurate behavior, not a bug - real
hardware booting from this exact flash state would report the same
thing. The `rootfs` partition (`0x640000`), by contrast, has a real
UBI header (`55 42 49 23` = `"UBI#"`) at the expected offset,
confirming the read path is byte-accurate against real data.

## 18. Known limitation: second normal-boot cycle hangs in malloc()

Normal (non-recovery) boot, when no valid kernel is found, cleanly
resets (section 14) and retries - but the *second* attempt hangs
inside u-boot's own `malloc()` (confirmed via `gdb-multiarch`: stuck
in a tight loop around `malloc()+0x21e`, PC in the `bic.w
r5,r5,#3`/`subs r7,r5,r4`/chunk-size-masking region of dlmalloc-style
free-list logic). Reproduced identically **twice**, independently, with
and without real NAND data backing (section 17b) - same heap address
involved both times (`r1=0x4a9a7dd0`), meaning it's a deterministic
corruption tied to the second boot cycle specifically, not randomness
or missing NAND data.

Root cause (understood, not yet fixed): `GCNT_PSHOLD` on *real*
hardware triggers an actual power-cycle - the DRAM controller re-inits
from scratch and RAM content is genuinely gone, not just CPU state.
This emulator's PSHOLD handler (section 14) only does
`qemu_system_reset_request()`, which resets CPU/device state but
*preserves* RAM content (QEMU's normal, correct behavior for a "warm"
reset - the mismatch is that PSHOLD isn't architecturally a warm
reset). Real hardware's SBL re-runs on every power-cycle, re-loading
appsbl into RAM and re-populating SMEM fresh before jumping in; this
emulator only does that *once*, at `mr80x_init()` (QEMU machine
creation), not on each subsequent reset. So the second boot cycle
starts with real hardware's assumption "RAM/heap is fresh" violated -
u-boot's own global data (BSS, malloc arena) still contains whatever
the first cycle's `malloc()` usage left behind, which apparently isn't
something the code tolerates.

**Why this doesn't block the emulator's actual purpose**: recovery
mode (`MR80X_RECOVERY=1`, sections 15-16) never reaches this code path
at all - it stays in the httpd receive loop indefinitely, and that's
the flow needed to test signed-image uploads. This only affects
"normal boot repeatedly failing to find a kernel and retrying," a
secondary scenario.

**Update - implemented the RAM-persistence fix, and it did NOT resolve
this bug**, which is itself an important, conclusive finding: moved
RAM-clearing + SMEM re-population + MVBAR trampoline re-write into
`mr80x_populate_ram()`, called from `mr80x_reset()` on every reset
including the implicit first one. The ELF/image itself is *not*
manually re-loaded there (QEMU hard-errors with "ROM images must be
loaded at startup" if you try) - instead, `mr80x_reset()` is now
registered *before* the one-time `load_elf_as()`/`load_image_targphys()`
call in `mr80x_init()`, so reset handlers fire in the right order every
cycle: ours (memset all of RAM to 0, then re-write SMEM/trampoline)
runs first, then QEMU's own internal `rom_reset()` (registered as a
side effect of the loader call, always after ours by registration
order) restores the loaded image on top of that clean slate. Verified
no regression: `MR80X_RECOVERY=1` + real NAND data + a
`tplink-cloud-sign.py`-signed upload still passes RSA verification and
returns "Upgrade Success" exactly as before (sections 15-17).

**But the second-boot-cycle `malloc()` hang is still there, at the
byte-identical PC and heap address** (`pc=0x4a93720e`,
`r1=0x4a9a7dd0`) as before this fix, confirmed via `gdb-multiarch`
immediately after implementing and rebuilding. Since RAM is now
provably, genuinely fresh (zeroed, then only SMEM/trampoline/ELF
re-written) on the second cycle, **stale first-cycle heap/BSS content
is conclusively ruled out as the root cause** - whatever's actually
wrong is something else, likely either a genuine pre-existing
fragility in this exact u-boot fork's `malloc()`/allocation pattern
under specific conditions only reachable on a *second* pass through
some init path (independent of memory freshness), or a subtler timing/
register-state difference in this emulator's own peripherals between
the first and second pass through the same code.

**Attempted follow-up**: traced every `malloc()` call (breakpoint at
its entry, `-S` so gdb attaches before the CPU ever runs, size + LR
logged automatically for 400 consecutive calls via a generated
`gdb-multiarch -x` script - `debug-session9.sh`/`gdbcmds.txt`) hoping
to catch an anomalous request size right before the hang. All 400
calls traced clean (small, sane sizes: 7-101 bytes, sensible-looking
LRs) with **no hang reached at all** within that trace - meaning
either the hang needs meaningfully more than 400 total `malloc()`
calls to reach, or (more likely) heavy breakpoint/`continue` stepping
via gdb itself perturbs timing enough that whatever triggers the hang
under free-running TCG doesn't reproduce the same way - a real
methodological wall for this specific bug, not just "hasn't been
tried yet." Not root-caused further within this session; would need
either a much longer trace, a conditional breakpoint closer to the
hang site itself (`*0x4a93720e`) instead of tracing from `malloc()`'s
entry, or a non-gdb approach (e.g. an in-emulator instruction counter/
log) that doesn't alter timing. Kept as a known, still-open limitation;
the RAM-repopulation-on-reset change is kept regardless since it's
still a real correctness improvement (accurately mimics a real
power-cycle) with zero observed regressions.

## 19. Real-time-accurate timer + more SMEM fakes - closing the "doesn't look like the real router" gap

The user pushed back hard on section 17's boot log, correctly pointing
out several messages that a real router wouldn't print on every boot.
Went through each one individually rather than assuming they're all
the same kind of issue:

- **"ipq_spi: SPI Flash not found"** and **"PCI1 is not defined in the
  device tree"**: NOT emulation bugs. Both come from driver probes
  reading the device tree blob *compiled into the appsbl binary
  itself* - identical bytes to what real hardware boots from. This
  board genuinely has no SPI-attached flash (only the QPIC-attached
  serial NAND) and its DTS genuinely doesn't define a second PCI
  controller - a real MR80X v5 printing these on its own serial
  console would be entirely expected, not a bug to "fix" here.
- **"smem: Get socinfo - version failed"** (x2) and **"No ART
  partition found"**: genuine emulation gaps, now fixed.
  `ipq_smem_get_socinfo_version()`/`_cpu_type()`
  (`arch/arm/cpu/armv7/qca/common/smem.c`) read SMEM type
  `SMEM_HW_SW_BUILD_ID` (137) into a `union qca_platform`, trying
  `sizeof(qca_platform_v1)`=72 bytes first -
  `smem_read_alloc_entry()` requires an *exact* size match against
  the alloc_info entry's declared size (not just "big enough"), so
  faked a 72-byte all-zero entry (both call sites only check the
  return status, not field values). Separately, added a `"0:ART"`
  partition table entry (real offset/size from the flash dump,
  BRINGUP-NOTES section 4b partition #9) alongside the existing
  `0:APPSBLENV`/`rootfs` ones - `board/qca/arm/common/ethaddr.c`'s
  `smem_getpart("0:ART", ...)` now succeeds. Confirmed: both "failed"
  messages are gone; "No ART partition found" is replaced by "eth0/
  eth1 MAC Address from ART is not valid" - the partition is now
  genuinely found and read (progress - a *different*, more accurate
  message), but the specific MAC-address bytes at the expected
  sub-offset within ART aren't validating, which may be a real gap
  in this specific captured dump (same class of issue as
  `appsblenv` being blank, section 17b) or a sub-offset this hasn't
  been chased down yet - not further investigated this pass.
- **"Hit any key to stop autoboot: 0" appearing immediately, no real
  chance to press a key**: a genuine, now-fixed emulation bug,
  separate from the malloc() hang (section 18). The generic timer
  (`MR80X_TIMER_BASE`, read by `read_counter()`/`get_timer()` in
  `arch/arm/cpu/armv7/qca/common/timer.c`) was modeled as a
  free-running counter that jumped forward by a large fixed step on
  *every* read - deliberately, so short hardware busy-wait polling
  loops (a clock-control busy bit, a NAND status register) would
  resolve in a handful of TCG instructions instead of real
  microseconds. But `get_timer()`-based *human-scale* waits use the
  exact same counter, including the multi-read
  `CONFIG_BOOTDELAY`-based autoboot countdown (this device's
  `CONFIG_TP_IMAGE` build uses `CONFIG_BOOTDELAY=1`, one second) -
  the artificial step made even *that* appear to have already
  elapsed on the very first read. Replaced with a counter driven by
  QEMU's own virtual clock (`qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)`),
  scaled to `GPT_FREQ_HZ` (240000, from the DTS `gpt_freq_hz`
  property baked into this exact binary) via `muldiv64()` - genuinely
  real elapsed wall-clock time, matching real hardware's own
  behavior. Confirmed: the countdown now shows "1  0" (a real,
  if short, 1-second window) instead of jumping straight to "0".
  Short hardware-poll loops are unaffected in outcome, just no longer
  artificially compressed to "already done" on the first read either
  - they now take correspondingly real (still tiny, microsecond-scale)
  time, same as a real chip would need.

Verified no regression across all of these changes together: recovery
mode + real NAND data + a `tplink-cloud-sign.py`-signed upload against
`out/appsbl-custom.bin` still passes RSA verification and returns
"Upgrade Success".

**Remaining, NOT fixed this pass**: `*** Warning - bad CRC, using
default environment` - confirmed (again) genuinely accurate: the
entire 512KiB `appsblenv` region in `FULL_FIRMWARE.bin` is `0xFF`
(erased), byte-for-byte, not just at the specific offset `readenv()`
happens to read from. Every *other* partition checked (`sbl1`,
`mibib`, `bootconfig`, `qsee`, `devcfg`, `cdt`, `appsbl`, `art`) has
real, mostly-non-`0xFF` content - `appsblenv` (and `training`, which
is *expected* to be blank/regenerated-per-boot cached DDR timing data)
are the outliers. This means the specific `FULL_FIRMWARE.bin` capture
in hand didn't preserve valid environment data for this unit, not
that this emulator is reading the wrong location - fixing the
*message itself* would mean fabricating plausible-but-fake env
content, which hasn't been done since it would misrepresent what's
actually known about the real device's real state.

## 20. MILESTONE: genuine interactive u-boot console access

The user reported that even after sections 17-19's fixes (real-time
timer, BQL-blocking `g_usleep()` removed from UART TX), pressing keys
during the "Hit any key to stop autoboot" window still never dropped
into the console - confirmed this class of fix (timing-dependent
input) is fundamentally unreliable through terminal -> docker engine
API -> container -> QEMU, regardless of how correct the underlying
timer/blocking fixes are, and pivoted to a timing-*independent*
solution instead of continuing to chase sub-second latency.

**Root cause, found via source reading, not guessing**: `tstc()`/
`getc()` for this UART driver do NOT poll `UART_SR`/`RXRDY` (what this
emulator modeled) at all - `ipq_serial_pending()`
(`drivers/serial/qca_uart.c`) calls `msm_boot_uart_dm_read()`, which
polls a completely different register, `UART_MISR`, for the
`RXSTALE` bit, and separately reads `UART_RX_TOTAL_SNAP` for a byte
count before ever touching the RX FIFO word. Since this emulator
never modeled MISR/RXSTALE at all (reads always returned 0, the
generic-stub default), `tstc()` reported "no key waiting" *forever*,
regardless of anything actually sitting in the RX buffer - explaining
why even a "guaranteed" pre-seeded keypress (an earlier attempt at
`MR80X_STOP_AUTOBOOT`, added but not yet working when first tried)
silently did nothing.

Fixed by modeling `UART_MISR` (aliases `UART_CR`'s offset, same
alias-pattern already used for `UART_SR`/`CSR`) to report
`RXSTALE` set whenever the RX buffer is non-empty, and
`UART_RX_TOTAL_SNAP` (aliases `UART_IRDA`'s offset) to report the
actual pending byte count. The existing RF/TF0 FIFO-word read already
happened to return the right *format* (byte in the low 8 bits, zero
elsewhere) for msm_boot_uart_dm_read()'s "all-zero word means not
ready" hardware-quirk workaround to not misfire.

**`MR80X_STOP_AUTOBOOT`** (env var, wired into `run.sh` as
`--stop-autoboot`): pre-seeds the UART's RX buffer with one byte
before the guest ever starts running, so `abortboot_normal()`'s very
first `tstc()` poll - which happens immediately, no delay needed -
already sees it and aborts autoboot instantly. Zero timing dependency,
unlike racing an actual keypress against a ~1-real-second window
through several layers of latency. This is now the *recommended* way
to reach the console, not just a fallback.

**Verified genuinely interactive, not just "reaches a prompt"**:
injected a real `printenv` command via the container's stdin after
the `IPQ5018#` prompt appeared, and got back the full, real default
environment (`bootcmd=bootipq`, `bootdelay=1`, `ipaddr=192.168.1.1`,
`ethaddr=00:11:22:33:44:55`, etc.) - full command round-trip through
the fixed RX path, confirmed working end-to-end, not just TX.

## 21. MILESTONE: root-caused and fixed the real normal-boot crash *and* the second-boot-cycle malloc() hang - both were the same bug

Section 17's "next, still open" and section 18's malloc() hang were two
symptoms of one root cause, found by working entirely without gdb this
time (the earlier gdb single-step trace through the trampoline, section
17/summary carryover, turned out to be internally contradictory - raw
`x/8xb` showed correct bytes but `x/2i` disassembly and register
behavior during `stepi` showed a zeroed trampoline; that contradiction
was itself the clue, resolved below) - instead using: QEMU's `-d int`
exception trace (non-invasive, doesn't perturb timing), direct
`fprintf(stderr, ...)` instrumentation added temporarily to
`mr80x_pshold_write()`, and - most decisively - temporary `printf("DBG:
...")` statements added directly to appsbl's own
`board/qca/arm/common/cmd_bootqca.c` (`do_bootipq()`), rebuilt via
`make build`, and reverted once the root cause was confirmed (`make
build` re-verified byte-identical to `reference/OpenWrt.mtd8.0-appsbl.bin`
afterward).

**Symptom, precisely**: normal boot printed nothing at all between
"Hit any key to stop autoboot" resolving and a second, silent
"U-Boot 2016.01 ... DRAM: 256 MiB" banner - no crash dump, no `bad_mode()`
panic text, no `GCNT_PSHOLD write` log line (confirmed via the
`fprintf` above: the PSHOLD reset path, section 14/18's original
suspect, was never touched). `-d int` showed only *one* `Secure Monitor
Call` exception in the entire session (a genuinely unrelated, very
early `smc` in `start.S`-era code, `pc≈0x4a920304`, before `board_init_r`)
- meaning `do_bootipq()`'s own `qca_scm_call()` never even reached its
`smc` instruction, let alone faulted. The appsbl-side DBG prints
pinned it exactly: `"DBG: about to call qca_scm_call"` printed, then
nothing - the reset happens *inside* `qca_scm_call()` → `is_scm_armv8()`,
before its `smc #0`.

**Root cause**: the MVBAR/SMC trampoline (section 14) lived at
`appsbl_entry - 0x1000`. That address is *inside* DRAM, in the same
1MiB MMU section as `CONFIG_SYS_TEXT_BASE` (so it's execute-permitted,
which was the entire reason it was picked) - but `ipq5018.h`'s own
memory-map comment (the ascii diagram above `CONFIG_SYS_INIT_SP_ADDR`)
shows that exact region, immediately *below* `text_base`, is where
u-boot's malloc heap (`CONFIG_SYS_MALLOC_LEN` = 756KiB), page table,
`gd`/`bd` structs and all three exception stacks live -
`reserve_uboot()`/`reserve_malloc()` in `common/board_f.c` confirm it
for the `GD_FLG_SKIP_RELOC` path this build uses:
`gd->start_addr_sp = CONFIG_SYS_TEXT_BASE;` then subtracted downward
from there. By the time boot reaches `do_bootipq()`, enough real
`malloc()` activity (env parsing, driver/DM init) has touched that
memory that the trampoline's 8 bytes - written once, at reset time, via
`cpu_physical_memory_write()` - get silently overwritten as ordinary
heap churn. Confirmed via gdb (`break is_scm_armv8` + `x/8xb` on the
trampoline address, using the *unstripped* `build/u-boot-2016/u-boot`
ELF for symbols since `appsbl.unpadded.elf` has no section/symbol
table): bytes were all-zero by the time execution reached that
breakpoint, despite being correct immediately after reset. This also
retroactively explains the confusing/contradictory gdb single-step
trace from the section-17 investigation: `x/8xb`'s raw byte read really
was seeing correct bytes (read *before* the heap had touched that
address in that particular gdb run's timing), while continuing further
before disassembling hit the same race the real, undebugged crash did.

A first relocation attempt - `appsbl_entry + 0xD0000`, chosen naively
as "comfortably above the loaded image" - was *also* wrong, and zeroed
by the same `is_scm_armv8()` breakpoint check: u-boot's BSS section
(`__bss_start=0x4A9AF298` to `__bss_end=0x4A9F84E0` in this build's
`u-boot.map`, ~295KiB) extends well past the raw image size the
estimate was based on, and BSS gets zeroed by the C runtime very early
- long before `is_scm_armv8()` runs.

**Fix**: moved the trampoline to `appsbl_entry + 0xD9000`
(`0x4A9F9000`) - past *both* the image and its BSS (`__bss_end`), with
~2.8KiB of margin, and confirmed via the same `u-boot.map` that
`reserve_mmu()`'s `GD_FLG_SKIP_RELOC`-path TLB/page-table placement
(`CONFIG_SYS_TEXT_BASE + mon_len`, rounded up to the next 64KiB) lands
at `0x4AA00000` - a full 1MiB section above this one, since `mon_len`
tracks `__bss_end` and rounding pushes it past this section's
`0x4A9FFFFF` boundary - so it doesn't reach back down into this
leftover space either. Still safely inside the one exec-permitted
1MiB section. Verified via gdb: bytes intact (not zeroed) at the
`is_scm_armv8()` breakpoint, unlike both earlier locations.

**Result, confirmed via real (non-gdb) boot tests**:
- Normal boot: `qca_scm_call()` now genuinely returns `ret=-95`
  (`-EOPNOTSUPP`) instead of never returning: `do_bootipq()` correctly
  takes the `do_boot_unsignedimg` path, attempts a real UBI read
  (fails to find a `kernel` volume - the pre-existing, separate,
  already-tracked "empty MTD device detected" gap, item 17 in the
  status list below), and falls through cleanly to
  `"Both image corrupted, Enter http firmware recovery mode!"` and a
  working HTTP recovery server - exactly matching real hardware
  behavior for a device with no valid kernel, not a crash.
- `reset` at the console (tested via `MR80X_STOP_AUTOBOOT` +
  FIFO-injected `reset\n`): now genuinely completes a full second boot
  cycle end-to-end (re-prints the banner, re-probes NAND, reaches
  autoboot, reaches the recovery HTTP server *again*) with **no
  hang** - meaning section 18's malloc() hang is fixed too, not just
  worked around. Root cause for *that* symptom, in hindsight: the
  trampoline's raw instruction bytes, sitting inside the malloc arena,
  were being misinterpreted as free-list chunk metadata by dlmalloc-style
  allocation/coalescing logic on whichever cycle's heap usage reached
  that address - explaining the "chunk-size-masking region" hang PC
  from section 18 without needing any RAM-staleness explanation at all.

Both of the user's two most recently reported symptoms - "ainda nao
inicia o kernel" (kernel still doesn't boot) and "o reset ainda nao da
reboot no uboot" (reset still doesn't reboot) - are the same bug and
are now fixed.

## 22. "ipq_spi: SPI Flash not found (bus/cs/speed/mode) = (0/0/48000000/0)" is expected, not a gap

Asked about during a normal-boot log review. Confirmed via source, not
guessed: `board/qca/arm/ipq5018/ipq5018.c`'s board-storage-init
function unconditionally probes *both* flash types the IPQ5018 family
supports - QPIC NAND (`qpic_nand_init()`, what this router actually
has) and SPI-NOR (`ipq_spi_init()`,
`drivers/mtd/ipq_spi_flash.c:130`, gated on the DTS's `/spi/spi_gpio`
node existing, which it does: `arch/arm/dts/ipq5018-soc.dtsi:36-40`
has `spi { ... status = "ok"; ...}` unconditionally, at the *SoC*
level, included by every board `.dts` including
`ipq5018-emulation.dts` (the one this build's machid,
`MR80X_TARGET_MACHID = 0x0F040000`, actually selects - see section 7b
for why the generic "emulation" DTS stands in for the real,
unavailable Mercusys/TP-Link OEM one).

The SPI *controller* being enabled in the DTS only means the SoC pin
mux/QUP peripheral is wired up - it says nothing about whether a
physical SPI-NOR chip is soldered to this specific board.
`spi_flash_probe(bus=0, cs=0, speed=48MHz, mode=0)` sends a JEDEC ID
read and gets no valid response, because there genuinely is no SPI-NOR
chip on this router (confirmed independently: all 16 real partitions,
section 4b, live in NAND; the real flash dump used as
`MR80X_NAND_IMAGE` is NAND-only). So `flash` comes back NULL and the
driver prints this exact message - real hardware over a TTL adapter
would print the byte-identical line, for the byte-identical reason.
Nothing to emulate here; an SPI-NOR chip that answered the probe would
be the *wrong*, unfaithful behavior.

## 23. MILESTONE: boot the APPSBL directly from `FULL_FIRMWARE.bin`

The machine no longer requires a separately-mounted
`appsbl.unpadded.elf`. With `MR80X_NAND_IMAGE` set and no `-kernel`,
`mr80x_init()` now performs the final handoff that the real proprietary
PBL/SBL/QSEE chain performs before entering u-boot:

1. validates that the full NAND image reaches the complete real `0:APPSBL`
   partition (`0x380000..0x4BFFFF`, size `0x140000`);
2. reads that exact partition and verifies its ELF magic;
3. passes the extracted slice through QEMU's normal ARM ELF loader, which
   loads the PT_LOAD segments at their linked addresses and registers them
   as ROM content restored after every reset; and
4. exposes the same `FULL_FIRMWARE.bin` simultaneously through the QPIC/BAM
   NAND model, so the executable APPSBL and every partition it subsequently
   reads come from one coherent flash image.

The real partition is an ELF padded with `0xFF` to its fixed partition size,
not a flat binary to copy wholesale to `0x4A920000`; using the ELF loader is
therefore important. The available dump's APPSBL slice is byte-identical to
`appsbl/out/appsbl.bin` (SHA-256 `1c8fbfd94bbb45fe8e1224dcca7686a6d27eb0a4bcc7edc3aee9faccc685951e`).
The loader reported 644170 bytes of actual ELF segments loaded.

`-kernel <elf-or-bin>` remains supported and deliberately takes precedence
as a development override, while `MR80X_NAND_IMAGE` still backs NAND reads.
Normal full-flash boot is now simply:

```sh
./run.sh
# or explicitly:
./run.sh --nand-image /path/to/FULL_FIRMWARE.bin
```

For a self-contained local workspace, keep the dump at
`qemu-ipq5018/images/FULL_FIRMWARE.bin`. The whole `images/` directory is
gitignored because the dump is 108265472 bytes and contains device-derived
flash data; `run.sh` prefers this repository-local copy, then falls back to
the original `openwrt-build-tools/.../fw_extracted/FULL_FIRMWARE.bin` path.
The local copy currently verified for this workspace has SHA-256
`69748b4392d116d54ac5fc30c63db45dc3fa56ec6cb704a60016e94250c647ed`.

This models the result of the pre-u-boot boot chain, not execution of PBL,
SBL1 or QSEE themselves. Executing those proprietary stages would require a
separate model for secure boot, TrustZone, DDR training, PMIC and other early
SoC facilities; it is not necessary for full fidelity of hardware as seen by
this u-boot.

### Reset regression found and fixed while validating this path

The first full-flash boot reached `IPQ5018#`, but issuing `reset` made the
second probe read NAND ID `0x0000`. RAM and APPSBL were already being restored
correctly; the problem was the emulator's QPIC register file and BAM pipe
descriptor/FIFO offsets surviving QEMU's system reset. Real controller state
is volatile across the router's power-cycle.

Added explicit QPIC NAND and BAM reset handlers. They clear controller
registers, per-pipe offsets and generic BAM registers, restore the QPIC
version register, and preserve only the NAND image pointer/size (the physical
flash contents). Verified end-to-end after rebuilding:

- first boot from the NAND partition identifies ID `c1c8`, reaches the real
  interactive `IPQ5018#` prompt, and reads the real flash backing;
- a console `reset` prints a second u-boot banner, identifies `c1c8` again,
  reads the environment again, runs the UBI scan and reaches recovery;
- the legacy `-kernel appsbl.unpadded.elf` override still reaches the same
  prompt with the full NAND attached; and
- `run.sh --no-net --stop-autoboot` works with no positional APPSBL argument,
  auto-detecting and booting `FULL_FIRMWARE.bin`.

## 24. UBI no longer appears empty; u-boot loads the kernel from rootfs

Normal autoboot from the full NAND image originally reached `ubi part fs`, but
UBI attached the partition as empty:

```
ubi0: empty MTD device detected
Read 0 bytes from volume kernel to 44000000
Volume kernel not found!
```

The image itself was not missing the kernel. `rootfs` starts at `0x640000` and
contains valid UBI metadata: EC header at `+0x0`, VID header at `+0x800`, and
the layout volume data at `+0x1000` naming `kernel` and `ubi_rootfs`.

The emulator had three NAND-read fidelity bugs that only became visible once
UBI scanned past the first header:

- BAM raw DATA/STATUS pipes were processed immediately when kicked, but this
  APPSBL queues DATA/STATUS before the matching CMD descriptor. Real BAM lock
  groups hold those pipes until CMD programs the QPIC address/read location.
- `NAND_ADDR0`'s low 16 bits were ignored. That broke OOB-style reads such as
  the bad-block marker at column `2048`.
- Multi-page reads were treated as a single infinitely long page. The driver
  actually streams `2048` main bytes plus the requested `16` OOB bytes, then
  advances to the next NAND page.

Fixed by recording DATA/STATUS pipe kicks as pending work, draining them only
after CMD has run, seeding the read stream from the `NAND_ADDR0` column, and
mapping the stream as `2048 + 16` bytes per page. OOB bytes are synthesized as
`0xff` because `FULL_FIRMWARE.bin` is a main-area dump without spare data.

Verified after rebuilding `mr80x-qemu:9.1.0`:

```
./run.sh --no-net --nand-image images/FULL_FIRMWARE.bin
```

The boot now reaches the real kernel handoff path:

```
ubi0: user volume: 2, internal volumes: 1
No size specified -> Using max size (3703796)
## Loading kernel from FIT Image at 44000000 ...
   Verifying Hash Integrity ... crc32+ sha1+ OK
   Uncompressing Kernel Image ... OK
Starting kernel ...
```

No kernel console output was observed in the short validation window after
`Starting kernel ...`; that is now the next boot-stage problem, separate from
u-boot locating and loading `kernel` from UBI.

## 25. The kernel genuinely runs (not stuck) - it was crashing on its own first SMC/PSCI call, fixed with QEMU's native PSCI instead of the guest trampoline

Follow-up to section 24's "no console output" note. First confirmed the kernel
isn't just silently hung: `-d int` exception tracing (non-invasive, doesn't
perturb timing the way gdb single-stepping does - see section 21's
methodology) shows real kernel code executing after "Starting kernel ..." -
`AArch32 mode switch from svc to irq/abt/und/fiq/svc PC 0x81312520..0x81312548`
is the kernel's own early per-mode exception-stack setup, standard ARM Linux
boot code, definitely not appsbl.

It then crashes: `Taking exception 3 [Prefetch Abort] ... IFSR 0x5 IFAR
0x4a9f9008` - a translation fault at the exact physical address of the
MVBAR/SMC trampoline (section 21). The kernel's device tree declares two CPUs
with `enable-method = "psci"` (`cpu@0`/`cpu@1` under `/cpus`, confirmed by
dumping the live in-RAM DTB via the QEMU monitor's `pmemsave` command mid-boot
and decompiling with `dtc` - see the exact recipe below) and a `psci` node,
so this is Linux's own PSCI probe/CPU_ON `smc` landing at the same Monitor
vector appsbl's SCM_SVC_FUSE calls do. Unlike appsbl - which runs under
u-boot's own unusually permissive "one whole 1MiB section is exec-friendly"
MMU setup (section 21) - the kernel builds its own page tables from scratch
and, for reasons not fully root-caused (neither the DT's `memory` node,
`reg = <0x0 0x40000000 0x0 0x10000000>`, nor its `reserved-memory` carve-outs
- `nss@40000000`, `smem@4ab00000`, `tz@4ac00000`, `tzapp@4a400000`, `bt@7000000`
- exclude this address), simply never maps the trampoline's page at all.

Recipe used to get the live DTB out of guest RAM for inspection (u-boot's own
"Loading Device Tree to 4a3ef000, end 4a3ff4f1" log line gives the address/
size):

```
qemu-system-arm -M mr80x ... -monitor unix:/tmp/mon.sock,server,nowait &
sleep 8   # long enough to be well past DT load, before/around the crash
printf 'pmemsave 0x4a3ef000 0x104f2 kernel_dtb.bin\n' | <send to the socket>
dtc -I dtb -O dts kernel_dtb.bin -o kernel_dtb.dts
```

**Fix**: rather than chase where the kernel's own paging_init() does or
doesn't map things (its page-table logic is arch/generic Linux code, not this
project's own, so there's no source here to grep the way appsbl's SCM path
could be traced in section 21), sidestep the whole "is this guest-RAM address
mapped by whichever page table happens to be active" question entirely by
using QEMU's own *native* PSCI implementation (`target/arm/tcg/psci.c` -
the same C-level SMC interception the `virt` machine type uses for SMP boot)
for calls made after u-boot's own handoff. QEMU intercepts a `psci_conduit`
CPU's `smc` *before* any guest instruction fetch happens for it at all, so
guest page tables become irrelevant.

Can't just set `cpu->psci_conduit = QEMU_PSCI_CONDUIT_SMC` unconditionally
from machine start, though: `arm_is_psci_call()` intercepts *every* `smc`
regardless of function ID (checked before the instruction executes) - it
would also swallow appsbl's own Qualcomm-specific SCM_SVC_FUSE calls. QEMU's
PSCI handler's "unrecognized function ID" case returns
`r0 = QEMU_PSCI_RET_NOT_SUPPORTED` (`-1`, `target/arm/kvm-consts.h`), which
appsbl's own `arch/arm/cpu/armv7/qca/common/scm.c`'s `scm_remap_error()`
maps to `-EIO` (`-1` matches `SCM_ERROR`, checked *before*
`SCM_EOPNOTSUPP = -4` in that switch) - not `-EOPNOTSUPP`, which is exactly
the value `do_bootipq()` branches on (section 21) - reintroducing the
original silent-reset bug this session already fixed once.

So it's gated dynamically in `board/mr80x.c`: a periodic `QEMU_CLOCK_VIRTUAL`
timer (`mr80x_psci_watch_tick()`, 5ms interval, pure QEMU-side polling, no
guest involvement) watches the CPU's PC and flips `psci_conduit` to `SMC`
the first time PC lands outside appsbl's own ~1MiB code footprint
(`< MR80X_APPSBL_ENTRY` or `>= MR80X_APPSBL_ENTRY + MiB`) - a simple,
FIT-image-independent proxy for "u-boot is done, this has to be the kernel
(or later)" that doesn't require hardcoding any specific kernel load address
(varies per FIT image/build). Reset back to `DISABLED` on every machine
reset (`mr80x_reset()`), so the console `reset` command's second boot cycle -
which re-runs appsbl's own SCM_SVC_FUSE calls from scratch - keeps working
exactly as it did before this existed (section 21).

Verified via `-d int`: the same PSCI-probe `smc` that used to fault now shows
`...handled as PSCI call` with no abort at all, confirmed across a fresh full
boot from `FULL_FIRMWARE.bin`.

**Still open**: even with the crash gone, no kernel console output has been
observed yet. Confirmed via `printenv bootargs` at the u-boot console:
`bootargs=console=ttyMSM0,115200n8` - the kernel *is* told to use the same
physical UART (`ttyMSM0`, the MSM/QUP serial block at `0x078AF000`) u-boot's
own console already uses, so this isn't a missing-`console=` problem. That
narrows it specifically to the Linux `msm_serial` driver's own register-level
expectations (interrupt-driven, DT-`compatible`-string-matched, quite
different code from u-boot's bare-metal `qca_uart.c` polling loop this
emulator's UART model was built against) not lining up with what this board
model provides - `-d int` shows no further exceptions after the PSCI fix,
meaning execution is proceeding normally, just silently as far as this
emulator's UART model can tell. This is meaningfully bigger in scope than everything
else in this file: it's the difference between "emulate what u-boot needs"
(this project's original, now largely complete goal - SPI/NAND geometry,
partitions, Ethernet, UART, recovery HTTP, signature verification, reboot)
and "emulate what a full Linux kernel needs" (earlycon/console driver
register semantics, GIC interrupt controller, ARM generic timer as the
kernel sees it - not just u-boot's simpler polling use of it, etc.) - a
materially larger, more open-ended undertaking than the bootloader-focused
scope this file has tracked so far.

## 26. MILESTONE: a real, fully-readable Linux dmesg - added the GICv2 and fixed multi-char UART writes

Picked up directly from section 25's "still open": chasing full kernel
console output past the PSCI fix. Two structural gaps, both now closed:

**1. No interrupt controller existed in this machine at all.** u-boot never
needed one (every driver it uses - `qca_uart.c`, `qpic_nand.c`, the GMAC
driver - is a pure polling loop, confirmed across this whole project), but
Linux's boot absolutely requires one: the CPU's built-in architected timer
(already correctly emulated by QEMU's cortex-a7 model - nothing to build
there) delivers its expiry as a PPI line *into* the GIC, and the kernel's
`msm_serial` console driver is interrupt-driven, unlike u-boot's own
`qca_uart.c`. With no GIC, both have nowhere to signal at all.

Added QEMU's existing, reusable `arm_gic` device (`hw/intc/arm_gic.c` -
the same GICv2 model `hw/arm/highbank.c` and others already use, no custom
device needed) in `mr80x_init()`, wired per the live in-RAM DT dumped in
section 25 (`interrupt-controller@b000000`, `compatible = "qcom,msm-qgic2"`):
GICD at `0xb000000`, GICC at `0xb002000`, single CPU. The DT's own
`timer { compatible = "arm,armv8-timer"; interrupts = <1 2 0xf08 1 3 0xf08
1 4 0xf08 1 1 0xf08>; ...}` node gives this *specific* SoC's PPI wiring for
the architected timer's four lines (secure-phys=PPI2/INTID18,
non-secure-phys=PPI3/INTID19, virtual=PPI4/INTID20, hyp=PPI1/INTID17) -
notably *not* the generic ARM "virt" machine's convention
(`include/hw/arm/bsa.h`: 29/30/27/26) other QEMU boards use, so those
constants don't apply here. All four wired regardless of which one the
kernel actually ends up using (harmless if unused - it isn't confirmed
whether this boot chain ever flips the CPU to Non-secure state at all).
The UART's own IRQ output (new, see below) is wired to SPI `0x6b` (107),
matching the DT's `serial@78af000`'s `interrupts = <0 0x6b 4>`.

**2. Even with the crash gone and the GIC in place, the very first kernel
output was scrambled garbage** - `[ 00oni pi` where a clean line should
read something like `[    0.123456] some message`. Root cause, found by
comparing against the raw byte dump (not the terminal-rendered text, which
was actively misleading): `mr80x_uart_write()`'s TF0 (transmit FIFO) case
only ever forwarded `(uint8_t)value` - the *low byte* of whatever was
written. u-boot's own `qca_uart.c` always writes one character per TF0
write (confirmed, already documented in that function's comment), so this
was never wrong for anything this emulator had produced output from before
- but real MSM UART DM hardware's TF register can pack up to 4 characters
into one 32-bit write for throughput, and the *kernel's* `msm_serial`
driver, now running for the first time, does exactly that. Three of every
four kernel console characters were being silently dropped, and the
*first* surviving byte of each group also depends on how many chars were
batched in the write before it - producing output that looked
superficially like "something is happening" (a red herring at first
glance) but was actually unrecoverably scrambled.

Fixed by tracking `UART_NCHAR` (`NO_CHARS_FOR_TX`, offset `0x40`) - real
hardware requires this written before each TF push to say how many of its
bytes are valid, and u-boot's own driver already always writes 1 before
each single-char TF write, so this needed no change on the u-boot side.
`mr80x_uart_write()`'s TF0 case now forwards `min(nchar_remaining, 4)`
bytes from the 32-bit value (low byte first, matching real hardware's FIFO
push order), decrementing the counter, instead of unconditionally just the
low byte.

**Result, verified via a real (non-gdb) full boot from `FULL_FIRMWARE.bin`**:
fully clean, readable kernel dmesg - real Qualcomm platform driver probing
(coresight/ETM, clk framework, GMAC/PHY, etc.), for many kernel-log seconds
of real boot activity. u-boot's own boot is unaffected (re-verified end to
end: reaches the `IPQ5018#` prompt exactly as before).

**New frontier, not a regression**: the kernel now hits a real, specific,
well-characterized crash instead of silence:

```
Unable to handle kernel NULL pointer dereference at virtual address 00000000
Internal error: Oops: 5 [#1] PREEMPT SMP ARM
CPU: 0 PID: 1 Comm: swapper/0 Tainted: G        W       4.4.60 #1
pc : [<814d85dc>]    lr : [<8165da20>]    psr: 60000113
```

happening repeatedly (same fault, different trace IDs) during early
platform-driver probing (`swapper/0`/PID 1, i.e. still single-threaded
kernel init, not yet a user process), shortly after a `coresight-etm4x:
probe ... failed with error -22` line and a `drivers/clk/clk.c:578`
`WARNING:` - suggestive of some driver's `probe()` proceeding past a failed
clock/resource lookup without checking it and dereferencing a NULL result,
though not confirmed since no symbol table matches this exact kernel build
(the `System.map`/`vmlinux` available elsewhere in this workspace are for
an unrelated, much newer aarch64 OpenWrt target - this device's own kernel
is 32-bit ARM, Linux 4.4.60, extracted only as a raw decompressed binary
from the FIT image, no debug symbols). Eventually panics
(`Kernel panic - not syncing: Fatal exception`) and reboots via a genuine
watchdog-style 5-second countdown, back to the u-boot banner - a clean,
real reboot cycle, not a hang.

**Update - resolved in section 27**: this exact crash was the CoreSight
NULL-pointer Oops, fixed by patching the CoreSight nodes out of the live
device tree before kernel handoff. The "iteratively disabling/stubbing
suspect DT nodes" guess above was correct.

## 27. Continuing past CoreSight: BAM probe fix, IRQ wiring, and the current frontier (kernel NAND DMA protocol)

Direct continuation of section 26, working iteratively per explicit
instruction: fix, rebuild, test, repeat on whatever crash/error appears
next, until real progress stalls.

**CoreSight NULL-pointer Oops - fixed.** Root-caused the crash flagged as
open in section 26. Since the device tree comes from inside the signed
FIT image (not this project's own source), the fix patches the *live*,
already-loaded DTB in guest RAM directly, triggered off u-boot's own UART
TX stream (the existing UART TX path now watches for the literal
"Starting kernel" string - the last thing u-boot ever prints - confirming
the DTB is fully placed and about to be handed off, no timing assumptions
or hardcoded addresses needed). No libfdt available to this build, so
this hand-rolls the flattened-devicetree structure-block format directly
in `mr80x_patch_fdt_disable_coresight()`/`mr80x_try_patch_fdt_at()`.

Getting this right took several real bugs, each found via temporary debug
instrumentation against a live boot (removed once confirmed working):

- A naive "first FDT magic in RAM" scan finds false positives before the
  real DTB: the kernel image itself (decompressed at a lower address)
  coincidentally contains the same 4 magic bytes somewhere in several MB
  of code/data, and separately, u-boot's own FIT image container (loaded
  whole, earlier, at `CONFIG_SYS_LOAD_ADDR`) is *itself* valid FDT-format
  data. Fixed by only accepting a candidate whose "compatible"/"model"
  property contains "ipq5018" - this board's real, independently-confirmed
  identity (section 25).
- An off-by-4 reading the FDT header: `off_dt_strings` is at byte offset
  12, not 16 (offset 16 is `off_mem_rsvmap`) - made every property-name
  string-table lookup resolve to garbage, so nothing ever matched
  "compatible" and nothing got patched, silently.
- CoreSight's TMC/funnel/ETM/replicator nodes don't have "coresight"
  anywhere in their "compatible" string at all - they bind through the
  generic ARM PrimeCell/AMBA bus via `compatible = "arm,primecell"` plus a
  numeric peripheral ID. Fixed by tracking (per DT node, via a small
  nesting-depth stack) whether the node carries any `"coresight-*"`
  property instead (true of every real CoreSight component, e.g.
  `coresight-name`, regardless of which bus binds it) and blanking that
  node's "compatible" value if so.

Verified: no more CoreSight probing in dmesg at all, no more panic. Kernel
now gets past *all* driver probing through to attempting to mount root.

**BAM probe (`bam-dma-engine`) failing with -EINVAL for every instance -
fixed for the QPIC/NAND one.** New symptom after CoreSight: `UBI error:
cannot open mtd rootfs`, traced back through `qcom-nandc 79b0000.qpic-nand:
failed to request tx channel` to `bam-dma-engine: probe of 7984000.dma
failed with error -22`. Root cause, found by reading the (newer, but same
`qcom,bam-v1.7.0` register layout - matches this exact DT node's
compatible string) `bam_dma.c` available elsewhere in this workspace:
`bam_init()` reads `BAM_REVISION` (offset `0x1000` for this layout) to
compute `num_ees` from bits `[11:8]`, and requires the DT's `"qcom,ee"`
value (0 here) be strictly less than it. Left at this model's all-zero
default for every unhandled BAM offset (falls through to
`generic_regs[]`), `num_ees` read as 0, so `0 >= 0` was always true and
probe always failed - for *every* BAM instance on the board, not just
QPIC/NAND's. Fixed by seeding `BAM_REVISION` (`num_ees=1`) and
`BAM_NUM_PIPES` (matching this model's real `MR80X_BAM_NUM_PIPES=4`) in
`mr80x_bam_reset()`.

Verified: the QPIC-NAND BAM instance (`7984000.dma`) now probes
successfully and `qcom-nandc` obtains a DMA channel - real progress from
"failed to request tx channel" to actually *submitting* descriptors, which
now fail for a different, deeper reason (below). Two other BAM instances
(`7884000.dma`, `704000.dma` - unrelated to NAND/rootfs, presumably
console-DMA and crypto-engine BAMs) still fail to probe; lower priority
since they don't block root mount.

**Current frontier: `qcom-nandc 79b0000.qpic-nand: Error in submitting
descriptor to write config reg`, then `failure submitting descs for
command 255/144`, ultimately still `UBI error: cannot open mtd rootfs`.**
Confirmed via temporary debug instrumentation that the kernel's "cmd" pipe
(index 2 - `dmas = <0x05 0x00 0x05 0x01 0x05 0x02 0x05 0x03>` /
`dma-names = "tx\0rx\0cmd\0status"` in the DT, the *same* 4 pipe roles and
indices appsbl's own driver already uses) sends descriptors that: carry
the `BAM_DESC_CMD_FLAG` bit this model checks for, decode to *plausible*
`cmd_element`-format `{addr_n_cmd, reg_data, mask=0xFFFFFFFF, ...}` tuples,
and target register addresses correctly within `[MR80X_NAND_BASE,
MR80X_NAND_BASE + MR80X_NAND_SIZE)` - i.e. this model's *existing*
`mr80x_bam_process_cmd_desc()` (built for appsbl's own hand-rolled
cmd_element usage) appears to already be structurally compatible with
whatever `write_reg_dma()`/`prep_dma_desc()` (in the reference driver, at
least) constructs, and should be applying the register writes correctly.

Also wired the BAM's own interrupt (SPI `0x92`, `dma@7984000`'s DT node)
to the GIC - the kernel's `bam-dma-engine` is interrupt-driven (unlike
appsbl's polling driver, which is why this stayed unwired through the
whole rest of this project), pulsed whenever a pipe's `irq_stts` becomes
set from descriptor processing, matching the reference driver's own
`bam_dma_irq()`/`process_channel_irqs()` (reads `BAM_IRQ_SRCS_EE` then
per-pipe `BAM_P_IRQ_STTS`/clears via `BAM_P_IRQ_CLR` - all three already
correctly modeled here for appsbl's own use). Confirmed this is a real,
correct fix in isolation, but a full boot test shows it alone doesn't
resolve the error - identical failure persists.

**Genuinely stuck without kernel source** at this exact point, unlike
every other issue in this file: the literal error string ("Error in
submitting descriptor to write config reg") doesn't appear in *any* Linux
kernel source tree available anywhere in this workspace - this device's
kernel is an older (4.4.60), downstream Qualcomm fork of `qcom_nandc.c`/
`bam_dma.c`, and only *newer* (mainline-adjacent, different wording and
probably different structure in places) versions of both files happen to
be present elsewhere in this workspace for unrelated projects. Everything
diagnosed so far in this section relied on those newer versions being
*close enough* to infer the general mechanism (register offsets, overall
probe/IRQ flow) - which worked for BAM_REVISION and the IRQ wiring, both
independently confirmable as correct - but the exact reason the *specific*
descriptor submission still fails needs either this device's *actual*
kernel source (not available) or further blind experimentation (e.g.
whether `write_reg_dma()`-equivalent code in *this* kernel batches
multiple register writes into fewer/larger cmd_element buffers than
`prep_dma_desc()` does, whether `BAM_P_SW_OFSTS` - read by
`bam_read_offset_update()` per appsbl's own driver, section 13/14, but not
obviously used by the reference `bam_dma.c` - matters to *this* kernel's
driver specifically, or something about multi-descriptor completion
ordering across the "tx"/"cmd" pipes this synchronous, single-kick-at-a-
time model doesn't capture).

## 28. MILESTONE: real hardware is AArch64 (Cortex-A53) - a modern, fully-sourced OpenWrt kernel boots this board almost to a shell

Prompted by the section 27 dead end (no source for appsbl's exact 4.4.60
kernel/BAM driver fork) - the user pointed out this workspace *also* has a
complete, modern OpenWrt build already targeting this exact device
(`bin/targets/qualcommax/ipq50xx/openwrt-qualcommax-ipq50xx-mercusys_mr80x-v5-*`),
with full driver source available (`build_dir/target-aarch64_cortex-a53_musl/
linux-qualcommax_ipq50xx/`). Using that instead sidesteps the "no source"
wall entirely - and, as it turns out, uncovers something more fundamental.

**Real IPQ5018 hardware is AArch64-capable (Cortex-A53), not AArch32-only
(Cortex-A7) as this whole project had assumed.** Two independent pieces of
evidence, found *before* writing any code: OpenWrt's `qualcommax` target
(covering ipq50xx/ipq60xx/ipq807x, a mature target with many real,
physically-tested devices, not experimental) builds `ARCH=aarch64
CPU_TYPE=cortex-a53` uniformly; and appsbl's own
`arch/arm/cpu/armv7/qca/common/scm.c` has a dedicated `jump_kernel64()`
function that uses an SCM call (`SCM_ARCH64_SWITCH_ID`/`SCM_EL1SWITCH_CMD_ID`)
specifically to switch out of AArch32 right before handing off to a 64-bit
kernel. The `[410fc075]` MIDR this project's own board model has printed
all along (`cortex-a7` in QEMU's `-cpu`) was never independently confirmed
against real hardware - it was this project's own emulated CPU identifying
itself, taken on faith early on. **appsbl itself is, and stays, permanently
AArch32 on real hardware too** - confirmed by checking: there is no armv8
Qualcomm/IPQ5018 board port anywhere in this project's vendor u-boot
source, only generic upstream armv8 support for unrelated vendors
(hisilicon/fsl-layerscape/zynqmp/tegra). Building "a 64-bit appsbl" was
considered and ruled out for two reasons: it wouldn't be faithful to real
hardware (which never does this either), and the SoC-specific low-level
code this whole project has been reverse-engineering (clocks, DDR, NAND,
GMAC) simply doesn't exist in armv8 form to build from.

Given this, decided to validate the *feasibility* of running a real,
modern kernel on this board's existing peripheral models *before*
investing in genuinely emulating appsbl's AArch32->AArch64 SMC-mediated
handoff (real, separate work, deferred) - by booting the OpenWrt kernel
+ device tree directly, bypassing appsbl/u-boot entirely, gated behind
new `MR80X_AARCH64_KERNEL`/`MR80X_AARCH64_DTB` env vars / `run.sh
--aarch64-openwrt` (`mr80x_init_aarch64_test()`, a fully separate code
path from the existing appsbl boot flow - zero risk of regression,
confirmed by re-testing the original 32-bit flow end to end unaffected).
Uses the OpenWrt *initramfs* build specifically (kernel with an embedded
rootfs) so it can reach a real userspace shell without needing working
NAND/UBI at all - decoupling "do the peripheral models work" from
section 27's still-open NAND DMA problem entirely.

**Build system**: `qemu-system-arm` (the `arm-softmmu` target this whole
project built until now) fundamentally cannot instantiate a `cortex-a53`
CPU at all ("unable to find CPU model") - not a machine/board issue, a
hard QEMU target-list limitation. Switched the Dockerfile to
`aarch64-softmmu` instead (a superset - includes every 32-bit ARM CPU
model too, built from the exact same `hw/arm/mr80x.c` via the same shared
`arm_ss` source set every other QEMU ARM board file uses regardless of
target), producing `qemu-system-aarch64` - `run.sh` updated accordingly.
Re-verified the existing 32-bit appsbl boot flow completely unaffected by
this switch.

**Getting the AArch64 CPU to actually start correctly took one real,
now-understood fix**: `-cpu cortex-a53,aarch64=on` alone reset the CPU
into EL3 (the highest, secure-monitor exception level) - real hardware's
own TrustZone/secure-monitor firmware (not something this project models)
would normally take over from there; without it, the very first thing
QEMU's own reset logic did was an "exception return" from EL3 down to EL0
(user mode, the *lowest* privilege level - useless for kernel boot) at
PC 0, which the AArch64 Linux boot protocol never expects (it requires
entry at EL2 or EL1) - the CPU then spun forever taking Undefined
Instruction exceptions at EL0. Fixed with `-cpu
cortex-a53,aarch64=on,has_el3=off`, the same kind of CPU property other
QEMU AArch64 boards (e.g. `virt`) set when not modeling secure/TrustZone
state - resets directly into EL2 AArch64, matching what the kernel Image
expects, confirmed via `-d int`: real kernel code executing immediately,
including a clean early HVC (hypervisor call) exception/return cycle
(the kernel's own EL2 "hyp stub" install, standard early boot).

**Two more real, concrete fixes needed to get *console output*, both
because this test path deliberately bypasses real u-boot's own DT
fixups**:

- The kernel hit `Kernel panic - not syncing: Failed to allocate page
  table page` (in `create_kpti_ng_temp_pgd`/`paging_init`) - traced via
  gdb + this exact kernel's own `vmlinux-initramfs.elf`/`System.map`
  (available locally, unlike section 27's dead end) to PC landing exactly
  at `machine_restart`'s trailing safety loop, confirming a real panic-
  triggered auto-reboot had fired. Root cause: the DT's `memory@40000000`
  node ships with `reg = <0x0 0x40000000 0x0 0x0>` - a **zero size**. Real
  u-boot always patches this field with the actual detected DRAM size
  right before booting (a standard, universal u-boot convention this
  bypassed path never runs) - with genuinely zero declared RAM, the
  kernel's memblock allocator had nothing to hand out. Fixed by hand-
  editing the size field to `0x20000000` (512MiB, matching
  `MR80X_RAM_SIZE`) via `dtc`.
- Even with that fixed, boot proceeded silently until the *same* kind of
  panic-reboot loop, discovered by attaching gdb again with the correct
  `vmlinux-initramfs.elf` this time: PC sat inside `tty_register_device_attr`
  and other perfectly ordinary-looking driver-registration code - i.e.
  *not* actually hung, just producing zero visible output because no
  console/earlycon had ever been configured. `CONFIG_CMDLINE=""` for this
  build (confirmed in the target's `.config`) and the DT's `chosen` node
  only has `bootargs-append` (a u-boot-side convention this bypassed path
  also never processes, same root issue as the memory node) - not a bare
  `bootargs`, so the effective kernel command line was empty. Fixed by
  adding `bootargs = "earlycon console=ttyMSM0,115200n8 ...";` - `msm_serial`'s
  earlycon variant registers itself keyed by the DT node's own
  `compatible` string (`OF_EARLYCON_DECLARE(msm_serial_dm, "qcom,msm-uartdm", ...)`,
  confirmed by reading the driver source directly) and is auto-selected via
  `/chosen/stdout-path` once the bare `earlycon` token is present - no
  address/compatible needed on the cmdline itself.

**Result, verified via a real (non-gdb) boot with both fixes applied**:

```
[    0.000000] Booting Linux on physical CPU 0x0000000000 [0x410fd034]
[    0.000000] Linux version 6.12.94 ...
[    0.000000] Machine model: Mercusys MR80X v5
[    0.000000] earlycon: msm_serial_dm0 at MMIO 0x00000000078af000 (options '115200n8')
...
[    2.700000] Freeing unused kernel memory: 11776K
[    2.700000] Run /init as init process
[    4.340000] init: Console is alive
...
[   14.850000] procd: - init -
```

`0x410fd034` decodes as MIDR part number `0xD03` - genuine Cortex-A53,
confirming section 28's opening finding directly from the CPU's own ID
register, not an assumption. The kernel reaches `procd: - init -` -
OpenWrt's own init system's *final* stage before launching userspace init
scripts and (normally) a login shell - a real, modern, correctly-sourced
Linux kernel booting deep into userspace on this board's existing
peripheral models (GIC + UART, both built for the *old* 4.4.60 kernel
originally) with **no changes to either model needed**. Remaining
warnings along the way are non-fatal and already-understood gaps, not
new blockers: `gpll0_main failed to enable!` (repeated
`clk-alpha-pll.c` warnings - this model doesn't provide a PLL lock-
detect status bit the *modern* mainline-style clk driver polls for,
unlike the old 4.4.60 kernel's own special-cased "dummy clocks for
emulation" registration path); `bam-dma-engine ... failed with error -22`
for the *other* two BAM instances (unrelated to console/boot, same
already-tracked gap as section 27); USB/ext4/jbd2 module load failures
(expected - this model doesn't have USB, and the initramfs doesn't need
ext4); assorted `deferred probe pending` lines (crypto engine, cpufreq,
smp2p-wcss, GMAC - none block reaching `procd: - init -`).

Test artifacts saved under `images/` (gitignored, like `FULL_FIRMWARE.bin`):
`openwrt-mr80x-v5-Image` (raw, decompressed kernel - `dumpimage -T flat_dt
-p 0` on the OpenWrt-built `...-initramfs-uImage.itb`, then `gunzip`) and
`openwrt-mr80x-v5.dtb` (extracted via `dumpimage -T flat_dt -p 1`, then the
two fixes above applied via `dtc -I dtb -O dts` / hand-edit / `dtc -I dts
-O dtb`). `run.sh --aarch64-openwrt` auto-detects both.

**Not yet done**: reaching an actual interactive shell prompt (very
close - `procd: - init -` is the last log line seen in a ~4 minute real-
time test window; hasn't been confirmed to actually complete, could
still hit something after) and, separately, genuinely emulating appsbl's
own AArch32->AArch64 handoff so the *real* (non-bypassed) boot chain
works end to end - this section's test path is deliberately a shortcut
around that, not a replacement for it.

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
6. [done] Real GMAC1 DMA TX/RX + `slirp` networking (section 11).
7. [done] QPIC NAND device ID detection via a real BAM cmd-pipe model
   (section 13) - device found and identified correctly, but real
   *page data* still isn't backed by anything.
8. [done] GPIO14/TLMM: normal boot by default (section 12), deliberate
   recovery mode via `MR80X_RECOVERY` (section 15b).
9. [done] MILESTONE: MVBAR/SMC trampoline (section 14) - normal boot
   survives past autoboot instead of crash-looping.
10. [done] MILESTONE (section 15): real recovery HTTP server reachable
    end-to-end from the host - `./run.sh --recovery <elf>` +
    `curl http://localhost:8080/` returns the genuine firmware-upgrade
    page. Needed: GMAC reset-poll fix (15a), `--network host` instead
    of docker `-p` (15c), correct guest IP (15d).
11. [done] MILESTONE (section 16): a `tplink-cloud-sign.py`-signed
    image, uploaded through the real HTTP recovery form against
    `out/appsbl-custom.bin`, passes real RSA-2048/PSS signature
    verification ("Firmware checking passed") - the original point of
    building this whole emulator.
12. [done] MILESTONE (section 17b): real QPIC NAND page reads, backed
    by `MR80X_NAND_IMAGE` (e.g. `FULL_FIRMWARE.bin`) via the BAM
    data-producer + status pipes. `readenv()` now reads genuinely
    real bytes (confirmed: APPSBLENV really is blank/`0xFF` in this
    dump, "bad CRC" is accurate; `rootfs`'s real UBI header reads back
    correctly at the right offset).
13. [done] Interactive TTL console readable - UART line-ending
    normalization (section 17a).
14. [done] Moved SMEM re-population + MVBAR trampoline rewrite into
    `mr80x_reset()` (section 18 update) - a real correctness
    improvement (every reset now mimics a power-cycle, not just the
    first boot), verified no regression. The `malloc()` hang itself
    turned out to have a different root cause, fixed in section 21.
15. [done] MILESTONE (section 20): genuine interactive u-boot console
    access - fixed the *real* RX-detection bug (`UART_MISR`/`RXSTALE`
    and `UART_RX_TOTAL_SNAP`, not `UART_SR`/`RXRDY` as originally
    modeled) and added `MR80X_STOP_AUTOBOOT`/`run.sh --stop-autoboot`
    for guaranteed, timing-independent console access. Verified with a
    real `printenv` command round-trip.
16. [done] MILESTONE (section 20 addendum): all 16 real partitions
    exposed via the SMEM fake (not just the 3 originally added
    piecemeal), and `run.sh` auto-detects `FULL_FIRMWARE.bin` so real
    NAND data is on by default. Verified via `smeminfo` at the console.
17. [done] MILESTONE (section 21): root-caused and fixed the real
    normal-boot crash - the MVBAR/SMC trampoline was placed inside
    u-boot's own malloc heap and got overwritten by ordinary heap
    churn before `do_bootipq()` ever used it. Relocated past both the
    loaded image and its BSS. Normal boot now cleanly reaches the
    recovery HTTP server every time, and `reset` at the console now
    completes a full second boot cycle with no hang (this also fixed
    section 18's `malloc()` hang - same root cause, not a separate
    bug). `smeminfo`/`ubi0`'s "empty MTD device detected" is still
    open, see #19 below.
18. [done] MILESTONE (section 23): boot source unified with the physical
    flash model. Without `-kernel`, the APPSBL ELF is loaded directly from
    the real `0:APPSBL` slice of `FULL_FIRMWARE.bin`, while that same image
    backs QPIC NAND reads. `-kernel` remains a development override. QPIC
    and BAM volatile state now also resets correctly; a second boot after
    the console `reset` command re-identifies NAND and completes normally.
19. [done] MILESTONE (section 24): normal autoboot from
    `FULL_FIRMWARE.bin` now attaches the populated UBI, finds two user
    volumes, reads the `kernel` volume, validates/decompresses the FIT
    image and reaches `Starting kernel ...`.
20. [done] MILESTONE (section 25): fixed the kernel's own crash on its
    first post-handoff `smc` (PSCI probe/CPU_ON, per the DT's two-CPU
    `enable-method = "psci"`) - was faulting with a translation fault
    at the MVBAR trampoline's physical address because the kernel's
    own page tables (unlike u-boot's) don't map it. Fixed via a
    PC-watching QEMU timer that switches to QEMU's *native* PSCI
    implementation once execution leaves appsbl's own code range,
    while appsbl's own SCM_SVC_FUSE calls (including on the console
    `reset` command's second boot cycle) keep using the guest
    trampoline as before. Verified via `-d int`: no more abort, PSCI
    calls now show `...handled as PSCI call`.
21. [done] MILESTONE (section 26): added a GICv2 (interrupt controller -
    u-boot never needed one, the kernel absolutely does) and fixed
    multi-character UART TF writes (kernel's `msm_serial` packs up to
    4 chars/write, unlike u-boot's always-1; was silently dropping
    3 of 4). Real, fully-readable kernel dmesg now flows for many
    seconds of genuine platform-driver probing.
22. [done] MILESTONE (section 27): root-caused and fixed the CoreSight
    NULL-pointer Oops by patching those DT nodes out before kernel
    handoff (debug-only silicon this emulator can't model). Also fixed
    the kernel's `bam-dma-engine` probe failing for every BAM instance
    (missing `BAM_REVISION`/`BAM_NUM_PIPES`) and wired the BAM's own
    interrupt to the GIC. Kernel now gets past all driver probing to
    attempting root mount.
23. `qcom-nandc`'s DMA descriptor submission to the QPIC/NAND BAM's
    "cmd" pipe fails for the *old* 4.4.60 kernel
    (`UBI error: cannot open mtd rootfs`) even though the descriptor
    content, addressing, and IRQ delivery all check out correctly
    against the closest available reference driver source (section 27
    has full detail). This device's *exact* old kernel source isn't
    available anywhere in this workspace to pin down the remaining gap
    precisely. Superseded in practical importance by #24 below - a
    modern, fully-sourced kernel is a better target going forward.
24. [done] MILESTONE (section 28): confirmed real hardware is
    AArch64-capable (Cortex-A53, not Cortex-A7 as this whole project
    had assumed - never independently verified before). A real, modern,
    fully-sourced OpenWrt kernel for this exact device (already built
    elsewhere in this workspace) now boots on this board's existing
    peripheral models - unmodified GIC/UART built for the old kernel -
    all the way to `procd: - init -`, OpenWrt's own init system's final
    stage before userspace, via a new isolated test path
    (`run.sh --aarch64-openwrt`) that bypasses appsbl entirely.
25. **Next, still open**: two things, in order of value - (a) confirm
    the AArch64 OpenWrt test path actually reaches a working shell
    (very close, not yet confirmed complete - section 28); (b)
    genuinely emulate appsbl's own AArch32->AArch64 handoff
    (`jump_kernel64()`/`SCM_ARCH64_SWITCH_ID`) so the *real*, non-
    bypassed boot chain (appsbl loading/verifying the kernel from NAND,
    same as the whole rest of this project) reaches this same modern
    kernel instead of the old, source-unavailable 4.4.60 one - the
    harder, more architecturally faithful piece deliberately deferred
    until (a) confirmed doing this was worthwhile at all.
26. NAND *write* path (`DATA_CONSUMER_PIPE`, index 0) still isn't
    driven - real flashing after signature verification (section 16)
    still fails with "Attempt to write outside the flash area". Lower
    priority since it doesn't block the recovery/signing test flow
    (the HTTP response is "Upgrade Success" regardless).
27. Once kernel handoff and NAND write both work: test `out/appsbl-dual-key.bin`
    (accepts either the original vendor key or the swapped-in custom
    one) and a full-size real firmware image, not just a small test
    payload.
