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

## 29. MILESTONE: the real appsbl `jump_kernel64()` SMC handoff works end to end - a genuine, modern AArch64 OpenWrt boots via the real, non-bypassed boot chain, all the way to `procd: - init -`

Implements the real (non-bypassed) AArch32->AArch64 handoff section 28
deferred: appsbl's own `arch/arm/lib/bootm.c` calls
`jump_kernel64(kernel_entry, ft_addr)` as the last thing it does for a
64-bit kernel - `noreturn`, and on real hardware TrustZone firmware
handles the SMC by dropping straight into AArch64, never returning to
the AArch32 caller.

**Confirmed working**, verified via a real boot test
(`MR80X_NAND_IMAGE=images/full_firmware_openwrt.bin`, no `-kernel`
override - the exact same "boot APPSBL from the real flash partition"
path every other milestone in this file uses):

```
Starting kernel ...
Jumping to AARCH64 kernel via monitor
mr80x: appsbl jump_kernel64() SMC intercepted (params at 0x4a8224f0) - kernel_entry=0x41000000 fdt=0x4a3f6000 - requesting AArch64 handoff reset
mr80x: entering AArch64 kernel at 0x41000000, x0(fdt)=0x4a3f6000
[    0.000000] Booting Linux on physical CPU 0x0000000000 [0x410fd034]
[    0.000000] Machine model: Mercusys MR80X v5
[    ...]
Run /init as init process
init: Console is alive
init: - watchdog -
init: - preinit -
[   11.910000] procd: - early -
[   12.560000] procd: - ubus -
[   12.770000] procd: - init -
[   14.670000] kmodloader: loading kernel modules from /etc/modules.d/*
```

- `0x410fd034` is the genuine Cortex-A53 MIDR (confirmed section 28) -
  appsbl handed off from real AArch32 execution to a real AArch64
  kernel, through the *actual* `jump_kernel64()` SMC path, not the
  isolated bypass test path.
- The kernel is real, modern, fully-sourced OpenWrt 6.12.94 for this
  exact device, loaded from a real UBI `kernel` volume inside the
  `rootfs` MTD partition, via appsbl's own real FIT loading/hash
  verification (`crc32+ sha1+ OK`) - not a `-kernel` override, not the
  isolated `--aarch64-openwrt` test path.
- `procd: - init -` is the exact same milestone section 28's isolated
  bypass test path reached - now reached via the genuine, non-bypassed
  boot chain instead.
- Real dmesg follows throughout (real DTB parse, real driver probing,
  `kmodloader`, `init:`/`procd:` stages) - a live, correctly-configured
  system, not a crash landing. The many `deferred probe pending`/
  `Unknown symbol`/clock-related WARNs along the way are expected
  peripheral-modeling gaps (this board's models were tuned for the old
  kernel/isolated test path), not handoff bugs - see "What's
  deliberately NOT done yet" below.

### `images/full_firmware_openwrt.bin`: why a new flash image was needed, and two more mistakes made building it

The real `FULL_FIRMWARE.bin`'s actual shipped kernel turned out to be
**32-bit ARM Linux-4.4.60** (confirmed by decoding its FIT image with
`mkimage -l`: `Architecture: ARM`, `Load Address: 0x41208000`) - i.e.
`jump_kernel64()` is dead code for this device's real, as-shipped
firmware; a 32-bit kernel never triggers the AArch32->AArch64 switch at
all. All of the earlier "confusing, contradictory" empirical results
logged in an earlier draft of this section were downstream of chasing a
handoff that could never fire against the real shipped kernel - not a
bug in the trampoline logic itself.

Fix (the user's suggestion): `tools/build_full_firmware_openwrt.py`
starts from the real `FULL_FIRMWARE.bin` and surgically replaces only
the **`kernel` UBI volume** (inside the `rootfs` MTD partition,
`0x640000`/`0x2A00000`) with a FIT image wrapping OpenWrt's own real,
already-built AArch64 kernel+DTB for this device. Everything else in
the flash image - env, appsbl itself, the `ubi_rootfs` volume, all
other partitions - is left completely untouched.

Mechanics (see the script's own docstring and comments for the full
detail, this is the summary):
- appsbl's `config_select()` (`board/qca/arm/common/cmd_bootqca.c`)
  picks a FIT config by **plain name lookup**
  (`fit_conf_get_node(fit, name)` - `CONFIG_FIT_BEST_MATCH` is *not*
  enabled in this build, so there's no device-tree compatible-string
  matching involved at all), reading the candidate name from a
  `config_name` property baked into appsbl's own compiled-in control
  DTB (`arch/arm/dts/ipq5018-emulation.dts`: `config_name =
  "config@emulation-c2";`). So the new FIT just needs a config node
  named exactly `config@emulation-c2` - confirmed via `mkimage -l` on
  the real vendor FIT that this is genuinely the config name real
  boots on this emulated board already select.
- UBI volumes are parsed/rebuilt by hand (EC header, VID header, CRC32
  algorithm - `crc = zlib.crc32(data) ^ 0xFFFFFFFF`, confirmed against
  real on-flash CRCs before trusting it) - no `ubireader`/`mtd-utils`
  reading tools were available, only `ubinize`/`mkfs.ubifs` (building
  tools). The real vendor kernel volume's already-used LEBs are reused
  directly; extra LEBs (OpenWrt's kernel needs many more - see below)
  are taken from confirmed-free PEBs elsewhere in the same partition
  (real UBI doesn't require a volume's LEBs to be physically contiguous
  - each LEB's own VID header carries its `lnum`, scanned independently
  at attach time) - the `ubi_rootfs` volume's own PEBs are never
  touched.
- `mkimage`/`dumpimage` (`apt install u-boot-tools`, or OpenWrt's own
  `staging_dir/host/bin` copies) build/inspect the FIT itself.

Two more real mistakes were made (and fixed) getting from "the handoff
fires" to "a real system actually boots", both caught by just running
the result and reading what it did instead of assuming success from
"it verified and jumped":

1. **Wrong source kernel.** The first version pulled the `kernel-1`
   image out of OpenWrt's `...-squashfs-factory.ubi`'s own `kernel`
   volume - a real, correctly-built FIT, but built to be paired with a
   *separate* `ubi_rootfs` UBI volume this script never populates. That
   boots and hands off fine, but then hangs forever at `Waiting for
   root device /dev/ubiblock0_1...` - not a handoff bug, just the wrong
   artifact for a "does the handoff work" test. Fixed by using OpenWrt's
   own `...-initramfs-uImage.itb` instead - a complete FIT (not
   extracted from a UBI volume, used directly) with the **rootfs
   embedded in the kernel Image itself**, needing nothing else - the
   same self-contained property the isolated `--aarch64-openwrt` test
   path (section 28) already relies on.
2. **Volume table left stale.** The initramfs kernel is ~16.6MB
   (gzip) vs. the original 3.7MB kernel, needing ~132 LEBs instead of
   the original 30 - comfortably fits in the partition's ~147 free
   PEBs, but the volume table's `reserved_pebs` field for the `kernel`
   volume was left at its old value (`50`) after only writing the
   extra LEBs' EC/VID headers. appsbl's own mini-UBI reader
   cross-checks each volume's VID headers (`used_ebs`) against the
   volume table's `reserved_pebs` and rejects the *entire* UBI image
   ("`UBI init error 22`", then "`empty MTD device detected`", "
   `Volume kernel not found!`", falling back to HTTP recovery mode) if
   they disagree. Fixed: `patch_vtbl_reserved_pebs()` updates both
   redundant copies of the volume table record for the `kernel` volume
   (recomputing that record's own CRC) to match the real new LEB count,
   every time the script runs.

Regenerate with `python3 tools/build_full_firmware_openwrt.py`, then
`MR80X_NAND_IMAGE=images/full_firmware_openwrt.bin ./run.sh` (or
`./run.sh --nand-image images/full_firmware_openwrt.bin`).

### Two real bugs found and fixed in the trampoline while chasing this

1. **A hex-digit transcription error**: `jump_kernel64()`'s SMC
   function ID was computed (during this session's earlier planning,
   well before any code was written) as `0x0210010f`, but
   `QCA_SCM_FNID(SCM_ARCH64_SWITCH_ID=1, SCM_EL1SWITCH_CMD_ID=0xf,
   SCM_OWNR_SIP=2)` = `((1<<8)|0xf) | (2<<24)` = `0x10f | 0x02000000` =
   **`0x0200010f`**, not `0x0210010f` - a `1`/`0` digit swap that
   propagated into the trampoline unnoticed. Found via a temporary
   debug trampoline variant that logs the real `r0` for any
   unmatched armv8-convention SMC to a scratch MMIO register
   (`mr80x_debug_fnid_write()`, since removed) - immediately showed
   `fn_id=0x200010f` on a real boot, confirming the fix.
2. **The two SMC calling conventions in this codebase's `scm.c` handle
   error-code remapping differently, and the trampoline's original
   single shared default return value broke one of them.** Legacy
   convention (`scm_call()` -> `__scm_call()` -> `smc()`) always has
   `r0=1` on entry (a fixed trap value, not a function ID - the real
   command lives in a memory-pointed `cmd_addr`), and `__scm_call()`
   itself calls `scm_remap_error()` on the trampoline's returned value,
   converting the raw SCM code `SCM_EOPNOTSUPP` (`-4`) into the C errno
   `-EOPNOTSUPP` (`-95`, since `EOPNOTSUPP` is `95` in this codebase's
   `errno.h` - *not* `4`). The armv8 convention (`scm_call_64()` ->
   `__scm_call_64()` -> `__qca_scm_call_armv8_32()`) does **not** call
   `scm_remap_error()` - whatever the trampoline returns in `r0` is
   used raw. The original single-default trampoline always returned
   raw `-4` for anything unrecognized; under the legacy convention
   that's correct (its caller remaps `-4` to `-95` itself), but under
   the armv8 convention (needed for `is_scm_armv8()` to ever answer
   "yes", itself required for `jump_kernel64()` to not immediately
   `hang()`) the caller compares the *raw*, unremapped `-4` against
   `-EOPNOTSUPP` (`-95`) and gets no match - so `do_bootipq()`'s
   `ret == 0 || ret == -EOPNOTSUPP` check silently failed for *every*
   SMC-based decision, including its very first one
   (`qca_scm_call(SCM_SVC_FUSE, QFPROM_IS_AUTHENTICATE_CMD, ...)`),
   leaving its `do_boot` function pointer `NULL` and skipping straight
   back to the `IPQ5018#` prompt with **zero error output** - this
   was the "silent stop, no crash, no explanation" symptom chased at
   length in an earlier draft of this section. Fixed: the trampoline
   now distinguishes the two conventions by checking `r0==1` first
   (legacy convention's fixed trap value, never a real armv8 fn_id)
   and returns the *already-remapped* `-95` for any unrecognized
   armv8-convention call, `-4` (unchanged) for the legacy default.
   `board/mvbar-trampoline.S` has the full annotated source.

### What's deliberately NOT done yet

- **The `ubi_rootfs` UBI volume is untouched** (still the old vendor
  32-bit rootfs) - harmless for this milestone because OpenWrt's
  kernel Image is itself an **initramfs** build (embedded rootfs, no
  separate mount needed) - matching how the isolated `--aarch64-openwrt`
  test path (section 28) already works. The kernel's own
  `UBI error: cannot open mtd rootfs, error -2` in dmesg is expected
  and harmless for that reason; it doesn't block init. A *real*
  working userspace via this path (not just confirming the handoff)
  would need a matching `ubi_rootfs` volume too - not attempted here.
- **`gpll0_main failed to enable!` / `wait_for_pll` WARN** - this
  board's GCC clock-controller stub (section on `mr80x_gcc_ops`) was
  built against the old kernel's clock request patterns; the modern
  kernel's `msm_serial`/clock driver polls a PLL-lock bit this stub
  doesn't drive correctly yet. Cosmetic for now (kernel keeps booting,
  tainted but not crashed) - worth fixing before chasing a full
  console/userspace boot via this path.
- Reached `procd: - init -` (matching section 28's own isolated-path
  milestone) and a second `kmodloader` pass beyond it in the longest
  test run so far, but a confirmed *interactive shell prompt* via this
  (non-bypassed) path hasn't been separately verified to complete -
  section 28's own "confirm the isolated test path reaches a shell" is
  still separately open too.

### Implementation (`board/mr80x.c` + `board/mvbar-trampoline.S`, no QEMU core source touched)

**What's implemented** (all in `board/mr80x.c` + a new
`board/mvbar-trampoline.S`, no QEMU core source touched - see below):

- The MVBAR/SMC trampoline (section 14) is extended from 2 to ~20
  instructions, assembled via the real ARM cross-toolchain already
  used to build appsbl itself
  (`mr80x-appsbl-builder:openwrt-gcc5.2-binutils2.24`,
  `arm-openwrt-linux-muslgnueabi-as`), not hand-derived - source in
  `board/mvbar-trampoline.S`. It recognizes two specific SMC function
  IDs by `r0` and falls through to a (calling-convention-aware, see
  "Two real bugs" below) default for everything else, preserving every
  prior SMC-related fix:
  - `is_scm_armv8()`'s own probe (fn_id `0x82000601`) - answered
    "yes, real armv8 TZ" (`r0=0`, `r1=1`) so `jump_kernel64()` doesn't
    just `hang()` before ever trying its own SMC (`scm.c`'s
    `is_scm_armv8()` caches this as `scm_version` forever after the
    first call, so this has to be true from the very first probe of
    the whole boot, not just right before `jump_kernel64()`).
  - `jump_kernel64()`'s own SMC (fn_id `0x0200010f` = `QCA_SCM_FNID(
    SCM_ARCH64_SWITCH_ID=1, SCM_EL1SWITCH_CMD_ID=0xf,
    SCM_OWNR_SIP=2)`) - `r2` holds the physical address of `scm.c`'s
    on-stack `kernel_params` struct (`reg_x0`=fdt address at offset 0,
    `kernel_start` at offset 72). The trampoline `str`s `r2` to a new
    dedicated MMIO register (`MR80X_HANDOFF_TRIGGER_BASE`,
    `0x0A000000`, previously-unused address space) instead of
    returning `SCM_EOPNOTSUPP`.
- `mr80x_handoff_trigger_write()` reads the two fields out of guest
  RAM, saves them in new board-global state, and calls
  `qemu_system_reset_request()` - matching `jump_kernel64()`'s own
  "this SMC never returns to its caller" expectation instead of
  trying to emulate a live in-flight AArch32->AArch64 `ERET`
  transition.
- `mr80x_reset()` checks a new pending-handoff flag first: if set, it
  does exactly what the already-working isolated AArch64 test path's
  `mr80x_aarch64_reset()` does (section 28) - `cpu_reset()` +
  `cpu_set_pc()` to the saved kernel entry + `x0`=saved fdt address -
  *without* re-running `mr80x_populate_ram()`, since the kernel Image
  + FDT appsbl's own (already-working) FIT-loading logic placed in RAM
  survive the reset untouched.
- The CPU model itself had to move from `cortex-a7` to `cortex-a53`
  (`mc->default_cpu_type`, plus `object_property_set_bool(cpuobj,
  "aarch64", true, ...)` in `mr80x_init()`) so the AArch64 register set
  actually exists in `cp_regs` to switch into - real hardware is the
  same single Cortex-A53 core throughout appsbl and the kernel, never
  an A7, so this is more accurate, not a divergence. Since
  `arm_cpu_reset_hold()` unconditionally sets `env->aarch64=true` on
  reset for *any* CPU with the AARCH64 feature bit (not gated by any
  property under TCG), appsbl's own AArch32 boot needs that feature
  bit *off* at every normal reset and *on* only for the handoff reset -
  done by calling QEMU's own `set_feature()`/`unset_feature()`
  (`target/arm/cpu.h`, already public) at the top of `mr80x_reset()`,
  dynamically, per reset. `ARM_FEATURE_EL3` is toggled the same way
  (on for appsbl, off only for the handoff reset, matching the
  isolated test path's `has_el3=off`).

**Why no QEMU core source was touched**: the original plan considered
emulating a live in-flight AArch64 `ERET`-style state transition
(QEMU's own reference sequence is in
`target/arm/tcg/helper-a64.c`'s `HELPER(exception_return)`), which
needs `cpsr_write_from_spsr_elx()` - `static`, not exported. That
turned out to be unnecessary: `jump_kernel64()` is the last thing
appsbl ever does, so nothing needs preserving across the transition,
and the reset-based approach above (already proven by the isolated
AArch64 test path) sidesteps the whole question. `set_feature()`/
`unset_feature()`/`arm_feature()` and `cpsr_write_from_spsr_elx()`'s
own two dependencies (`aarch32_cpsr_valid_mask()` - `static inline` in
`target/arm/internals.h` - and `cpsr_write()` - public in
`target/arm/cpu.h`) are all already reachable from board code with no
core-source patching, for what it's worth.

**Confirmed working end to end** - see the log excerpt and
`images/full_firmware_openwrt.bin` explanation at the top of this
section. Builds cleanly; the normal AArch32 appsbl boot (NAND ID, env,
network) shows no regression from the `cortex-a7` -> `cortex-a53`
switch, and the real `jump_kernel64()` handoff, real UBI/FIT loading,
and real AArch64 kernel execution have all been directly observed in
one boot.

**Tooling note for next time**: gdb-multiarch could not be gotten to
attach usefully to this target while chasing the two bugs above - the
`aarch64-softmmu` build's gdbstub reports an AArch64-shaped register
file (`g` packet) even while the guest CPU is actually executing
AArch32 code, and `set architecture aarch64` connects but then reports
`pc=0` immediately and inserted breakpoints don't fire correctly. What
actually worked: QEMU-side `-d int` (exception tracing - confirmed
exactly how many SMCs fire and that returns are clean, not aborts) and
a throwaway debug trampoline variant that STRs the real `r0` of any
unmatched SMC to a scratch MMIO register logged via `info_report()` -
this is what found the `0x0200010f` vs `0x0210010f` bug directly,
after guessing at the value from source reading alone had failed
silently. Prefer this pattern over gdb for any future mixed 32/64
`cortex-a53` debugging on this board.

## 30. GCC clock branch/PLL WARN cleanup after section 29's real boot, and a genuine (unrelated) kernel bug found past `procd: - init -`

Section 29's real boot reached `procd: - init -` but with a wall of
`WARN` traces along the way (`clk_branch_toggle`: `"<clock> status
stuck at 'on'"`, `wait_for_pll`: `"gpll0_main failed to enable!"`) -
the old GCC clock stub (`mr80x_gcc_ops`, originally scoped to "only what
`uart1_clock_config()` touches", back when the old 4.4.60 kernel never
probed real clock branches/PLLs at all) doesn't synthesize the two
extra register conventions the modern kernel's real `clk-alpha-pll.c`/
`clk-branch.c` drivers actually use.

**Fixed** (`mr80x_gcc_read()`/`mr80x_gcc_write()`, see their own block
comment for the full detail): CBCR branch-clock registers (bit 0
driver-written CLK_ENABLE, bit 31 hardware-status CLK_OFF) and PLL_MODE
alpha-PLL registers (bits 1-2 driver-written BYPASSNL/RESET_N, bits
30-31 hardware-status ACTIVE_FLAG/LOCK_DET) now synthesize instant
success, matching this stub's existing "no real clock tree, report done
immediately" philosophy already used for CMD_RCGR's UPDATE bit.
Deliberately scoped to the *specific* register offsets a real boot's
WARN traces named (5 CBCRs: `gcc_cmn_blk_ahb_clk`/`_sys_clk`,
`gcc_qpic_clk`/`_ahb_clk`/`_io_macro_clk`; 4 PLL_MODEs: `gpll0_main`/
`gpll2_main`/`gpll4_main`/`ubi32_pll_main` - offsets from
`gcc-ipq5018.c`), not a blanket rule over the whole 1MiB block - a
blanket first attempt corrupted something else in the block, silently
breaking the AArch64 handoff entirely (appsbl reset-looped forever,
`Booting Linux on physical CPU` never printed once), caught by a clean
revert/retest A-B comparison. **Also had to narrow the pre-existing
"always clear bit 0 on read" rule** (originally meant only for
CMD_RCGR's UPDATE bit, applied blanket before this section since it was
"harmless" for the old kernel's much narrower register usage) to
exclude these new CBCR/PLL_MODE offsets - bit 0 there is real,
driver-meaningful state (CLK_ENABLE) that must read back as written;
blanket-clearing it corrupted Linux's regmap caching (a disable's
read-modify-write believed the bit was already clear and never
re-issued the actual write, so the bit-31 synthesis never got a chance
to run) - found via temporary read/write MMIO tracing on these exact
offsets, not guessed.

Verified via a real boot: zero clock-related WARNs, `procd: - init -`
reached consistently (multiple back-to-back runs).

**A separate, genuine problem found past that point - confirmed root
cause, not a board emulation gap, fixed without touching the OpenWrt
build tree**: `kmodloader`'s normal boot-time module autoload crashed
loading `vxlan.ko` - `Unable to handle kernel access to user memory
... vxlan_init_net+0x20`, a NULL-pointer deref reading `net->gen`.

Root-caused via disassembly, not guessed: `vxlan.ko` reads `net->gen`
at struct offset `2760`, but `gdb`'s own debug-info query against the
*current* `vmlinux-initramfs.debug` (`p (long)&((struct net*)0)->gen`)
gives `2368` - a real, ~400-byte **struct-net ABI mismatch between this
prebuilt `vxlan.ko` and the kernel it's loaded into**. Confirmed via
file timestamps: `vxlan.ko` in this OpenWrt tree's `staging_dir` is
dated well before the `vmlinux` it now ships alongside (the kernel got
rebuilt/relinked at some point after `kmod-vxlan` was last packaged,
without the module being regenerated to match) - a stale-module/build
staleness issue in the OpenWrt tree, not a bug in vxlan's source, not a
board-emulation gap, and *not fixed by rebuilding it* (attempted, but
that touches the OpenWrt package/build system in ways outside this
project's scope - reverted; no source files changed, only gitignored
`build_dir`/`staging_dir` build-artifact state, confirmed via `git
status` before backing out).

The same mismatch affects **every** module using per-namespace
generic pointers (`net_generic()`/`register_pernet_{subsys,device}`
with `.id` set) - not just vxlan; `x_tables`, `nfnetlink`, `nf_nat`,
`nf_tables`, `nf_conntrack`, and `ppp_generic` all hit the identical
crash in turn once the previous one was blocked, confirmed via
disassembly of each (`net.c` uses `struct net`'s `.deps`/generic-array
field the same way every time - the "Code:" bytes in each panic dump
decode to the exact same `ldr x0, [x0, #2760]` / `ldr xN, [x0, w1,
uxtw #3]` pattern `ops_init()` produces when it calls into
`net_generic()`).

**Fixed** (`tools/build_full_firmware_openwrt.py --extra-bootargs`,
default now `module_blacklist=vxlan,x_tables,nfnetlink,nf_nat,
nf_tables,nf_conntrack,ppp_generic`): appends to the DTB's
`chosen/bootargs-append` property (via a `dtc` decompile/edit/
recompile round-trip, the same technique already used for Option B's
DTB patches) - `module_blacklist=` is a real, mainline kernel
`core_param` (`kernel/module/main.c`, checked directly in
`load_module()`, so it blocks `insmod`/`kmodloader`'s syscall path,
not just in-kernel `request_module()`). Everything depending on a
blacklisted module fails to load too, but gracefully
(`kmodloader: dependency not loaded x_tables`, not a crash) - expected,
since none of iptables/nftables/PPP/VXLAN functionality is needed for
this milestone (confirming the AArch64 handoff and reaching a stable,
running system).

One real bug caught building the fix itself, worth remembering: the
first version inserted the blacklist string *before* the DTB
property's existing value (right after the opening quote) instead of
before the closing quote - appsbl's own `set_fs_bootargs()` (section
14/`board/qca/arm/common/cmd_bootqca.c`) concatenates its own base
cmdline directly onto this property's value with **no separator of its
own**, relying entirely on the value's own leading space; inserting
before it glued straight onto the base cmdline's last word with zero
space (`"...rootwaitmodule_blacklist=vxlan"`, confirmed on a real
boot - a single invalid parameter token, silently ignored by the
kernel, so it looked like nothing happened at first). Fixed by
inserting right before the closing quote instead, with an explicit
leading space of its own regardless of what precedes it.

**Verified via a real boot**: zero `Oops`/`Kernel panic`/reboots across
a 200-second run (previously crash-looped every ~18s, forever) -
`kmodloader`'s full module pass completes (with graceful
"dependency not loaded" skips for the blacklisted modules' dependents),
`zram0` swap comes up, and `urngd` (a normal OpenWrt userspace daemon)
starts at ~145s - genuine, sustained forward progress into userspace,
not just reaching `procd: - init -` and dying moments later.

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
28. [done] MILESTONE (sections 29-30): Option A, the real appsbl
    `jump_kernel64()` handoff, confirmed working end to end - a real,
    modern, fully-sourced AArch64 OpenWrt kernel now boots via the
    genuine, non-bypassed appsbl boot chain (real NAND/UBI/FIT
    loading, real SMC-mediated AArch32->AArch64 switch), using a new
    `images/full_firmware_openwrt.bin` (built by
    `tools/build_full_firmware_openwrt.py`) that swaps only the real
    flash image's `kernel` UBI volume for OpenWrt's own real AArch64
    initramfs kernel+DTB. All GCC clock-controller WARNs fixed
    (section 30); a genuine stale-module/kernel ABI mismatch in this
    OpenWrt tree (several netfilter/PPP/vxlan kmods, confirmed via
    disassembly) worked around via `module_blacklist=` on the kernel
    cmdline. Verified stable for 200+ seconds with zero crashes -
    reaches `procd: - init -`, `zram0` swap, and `urngd` running in
    userspace. Not yet separately confirmed: a full interactive shell
    prompt via this same path - see section 29's "What's deliberately
    NOT done yet".
29. [done] MILESTONE (section 31): two more real probe failures fixed
    (BAM crypto instance's `qcom,ee` mismatch; CMN PLL never modeled at
    all). Five remaining post-`procd: - init -` issues investigated and
    triaged - all confirmed either non-fatal/cosmetic or out of this
    project's scope (real clock-tree Hz modeling, a new DMA-engine
    protocol, or WiFi-coprocessor firmware loading), not quick fixes.
    Initially (wrongly) chalked up the missing shell prompt to rootfs
    config - see #30/section 32, that call was wrong.
30. [done] MILESTONE (section 32): found and fixed a real UART RX bug -
    the receive-FIFO register only ever returned 1 byte per 32-bit
    read instead of the up-to-4-bytes-packed the kernel's
    `msm_serial.c` actually expects (the exact mirror of a TX-side bug
    section 26 already fixed), which desynced the driver's own byte
    count the moment 2+ bytes arrived in one burst - orphaning a byte,
    keeping the level-triggered RX IRQ stuck asserted, and wedging the
    guest kernel in an infinite reentrant interrupt-handler loop
    (100% CPU, everything starved). This is the real mechanism behind
    "no keypress, not even Enter, ever reaches the console" - fixed
    and verified via a 260+ second boot sending 21 Enter presses plus
    a command string with zero crashes/hangs. A separate, still-open
    question (no shell prompt observed even now) was traced one level
    further to `/sbin/askfirst` (procd) blocking in `getchar()` behind
    a probably-unflushed stdio buffer - understood but not fixed.
31. [done] MILESTONE (section 33): built and verified real-production-
    rootfs UBI-image tooling (`--openwrt-squashfs-factory`), found and
    fixed two real bugs in it along the way (shrinking a UBI volume
    left stale valid-looking LEBs of its old, larger size behind;
    dynamic-volume VID headers need `used_ebs=0`, not the volume's
    real LEB count - both confirmed against the real vendored U-Boot
    UBI source, not guessed). A real, modern kernel+real squashfs
    rootfs UBI-attaches cleanly for the first time - but still can't
    mount, conclusively proven to be *entirely* downstream of the
    already-known, already-deferred `qcom_snand` runtime-NAND-driver
    gap (section 31), not a new bug and not fixable by any amount of
    correct UBI-image construction. Confirms the initramfs recovery
    image (section 32's still-open console mystery) remains the only
    currently-viable path to a running userspace at all.
32. [done] MILESTONE (section 35): missing console prompt fixed. The
    section 34 `rdinit=/bin/sh` dead end had already proven RX bytes
    were suspect; UART tracing then showed the opposite: the kernel was
    receiving and draining RX perfectly. The missing piece was the
    *TX* side used by normal TTY/userspace output: `printk`/console
    polling wrote directly, but shell echo/banner/output waits for the
    `msm_serial` driver's interrupt-driven TX path, which needs
    `TXLEV` in `MISR` when `IMR.TXLEV` is enabled. Modeling IMR/MISR
    plus `TXLEV`, and fixing `NCF_TX` reads so they no longer alias to
    RX FIFO, makes both `rdinit=/bin/sh` and the normal OpenWrt
    initramfs image fully interactive. Verified with `echo TXLEV_OK
    && uname -a` under rdinit and with the normal image opening
    `root@OpenWrt:~#`.
33. **Next, still open**: the remaining major hardware gaps are now
    the already-triaged runtime peripherals, especially Linux
    `qcom_snand`'s DMA-engine protocol (needed for any non-initramfs
    rootfs), MDIO/GMAC clocking/PHY details, and WiFi remoteproc/ath11k
    firmware loading. The real APPSBL -> UBI FIT -> AArch64 OpenWrt
    initramfs path is interactive.

## 31. Two more real probe fixes, and triaging what's left after `procd: - init -`

Continuing past section 30's `procd: - init -` milestone with the same
"test a real boot, fix what genuinely blocks or crashes, defer what
doesn't" approach:

**Fixed - BAM crypto instance** (`bam-dma-engine 704000.dma-controller:
… -22`): the generic BAM probe-only stub added in section 27/28
(`mr80x_bam_stub_ops`, `BAM_REVISION` reporting `num_ees=1`) covered the
SPI/I2C BAM instance fine, but the *crypto* BAM instance's DT node has
`qcom,ee = <1>;` - `bam_init()` (`drivers/dma/qcom/bam_dma.c`) rejects
whenever `bdev->ee >= bdev->num_ees`, so `num_ees=1` fails for `ee=1`
even though it was already enough for `ee=0`. Fixed by raising the
stub's reported `num_ees` to 8 (`8u << 8`, matching the real
`NUM_EES_SHIFT`), safely covering both instances' `qcom,ee` values with
one shared stub.

**Fixed - CMN (Common) PLL never modeled**: `clk_cmn_pll_recalc_rate`
WARNs on every boot - this SoC-wide reference-clock PLL
(`drivers/clk/qcom/ipq-cmn-pll.c`, DT `clock-controller@9b000`) had no
QEMU model at all. Added `MR80XCmnPllState`, a small new peripheral at
`0x0009B000` with power-on defaults matching what the driver expects
already locked (`CMN_PLL_LOCKED` bit 8 set, sane `REFCLK_CONFIG`/
`DIVIDER_CTRL` reset values from the real register field layout) - same
"report success/sane-defaults immediately, no real analog PLL
simulation" philosophy as the rest of this stub-heavy clock modeling.

**Triaged, confirmed out of scope for a quick fix** (investigated each
down to its real root cause in the modern kernel's own source, not
guessed - none of these block boot or crash anything, all are silent/
graceful `-EPROBE_DEFER`-style failures):

- `qcom_snand` (`-110`, NAND probe): the modern kernel's NAND driver
  (`drivers/spi/spi-qpic-snand.c` + `drivers/mtd/nand/qpic_common.c`)
  talks to the *same* QPIC BAM hardware this project already models,
  but via Linux's generic DMA-engine async-descriptor protocol - a
  different client protocol than the old u-boot bare-metal driver this
  project's existing BAM/NAND model was built against. Needs new
  reverse-engineering work comparable in size to the original BAM/NAND
  modeling effort - not attempted.
- `ipq4019-mdio` (`-22`, MDIO bus probe): `ipq4019_mdio_set_div()`
  requires `clk_get_rate()` to return a real, specific Hz value that
  matches one of a fixed set of divisors - this stub only synthesizes
  register *bit patterns* (PLL locked, CBCR enabled), it doesn't
  compute real clock-tree frequencies (PLL rate × RCG divider chains).
  Needs genuine Hz-accurate clock-tree modeling - a bigger task, not
  attempted.
- GMAC (`stmmac`) "failed to parse stmmac dt parameters": the
  `eth_wake_irq`/`sfty` "IRQ not found" messages right above it are
  *expected*, not bugs (the real device's own DT doesn't declare these
  optional IRQs either) - the actual probe failure is downstream of
  MDIO not coming up (no reachable PHY), so it should clear on its own
  once/if MDIO above is ever tackled. No standalone fix needed.
- `qcom-q6-mpd` remoteproc (WiFi coprocessor) probe failure: this
  driver (`drivers/remoteproc/qcom_q6v5_mpd.c`) needs a reserved
  `memory-region`, `firmware-name`, multiple clocks/power-domains, and
  ultimately real firmware-image loading (ELF/MDT parsing, the Q6's
  own boot protocol) to probe at all - it's the entire WiFi-radio
  firmware-loading subsystem, not a peripheral-register gap. Well
  outside this milestone's scope (confirming stable AArch64 boot into
  userspace); deferred as its own, much larger future project if WiFi
  emulation is ever pursued.
- Thermal zones (4×, `-110`): `tsens.c`'s `get_temp_common()` polls a
  "valid" status bit via `regmap_field_read_poll_timeout()` that times
  out since TSENS isn't modeled; found the DT node
  (`thermal-sensor@4a9000`) but not yet the exact bit-level "valid"
  flag layout. Purely cosmetic - doesn't block or slow boot. Lowest
  priority of everything found, left alone.

**Checked - no interactive shell prompt appears, initial pass**: ran
two more real boots after all the above fixes, one idle for 150s and
one sending newlines + a test command over the serial console after
the last log line, neither producing any `ash`/BusyBox prompt or
command echo. At the time, this looked consistent with this specific
initramfs image being a stripped-down recovery/test build whose
`/etc/inittab` doesn't spawn a shell on the console - **this
assessment turned out to be wrong, see section 32**: it wasn't a
rootfs configuration detail, it was a real, previously-undiscovered
UART hardware-emulation bug that this same "no prompt" symptom was
actually hiding.

## 32. MILESTONE: fixed a real UART RX bug that made keyboard input hang the kernel outright - "not even Enter works" was a genuine hardware-emulation gap, not a rootfs config detail

Section 31 signed off on the missing shell prompt as an inittab/rootfs
characteristic. The user then reported empirically that **no key at
all** - not even Enter - ever reached the console, which doesn't match
"no getty configured" (that would still show *something*, like an
unresponsive but stable prompt) - it matches something actively
broken. Investigated properly this time instead of re-asserting the
earlier (wrong) call.

**Root cause, confirmed via temporary instrumentation on a live boot,
not guessed**: the UART model's RF (receive FIFO) register read
handler (`mr80x_uart_read()`, offsets `UART_TF0`/`+4`/`+8`/`+12`,
aliasing the same address as TF - see the file's own register-map
comment) only ever popped **one** byte per 32-bit-wide read, zero-
padding the rest. Section 26 (`BRINGUP-NOTES.md`) already documents
the exact symmetric bug on the *transmit* side and fixed it there
(the kernel's `msm_serial.c` packs up to 4 characters per 32-bit `TF`
write) - but the fix was never mirrored onto the *receive* side,
because until now nothing had ever driven multi-byte RX through this
model (u-boot's own `qca_uart.c` RX path is a simple one-byte-at-a-
time poll loop, so section 20's interactive u-boot console never
exercised this).

The kernel's real RX consumer, `msm_handle_rx_dm()`
(`drivers/tty/serial/msm_serial.c`), reads `UARTDM_RX_TOTAL_SNAP`
*once* for the total pending byte count, then loops reading 32-bit
`RF` words, unconditionally treating **each** word as carrying up to 4
real bytes (`r_count = min(count, 4)`) - there is no separate signal
for "how many bytes did this specific read actually return". Give it
back only 1 real byte per word (as this model did) and its own count
bookkeeping goes out of sync the moment 2+ bytes arrive in a single
burst: it thinks it drained 4 bytes when it only got 1, silently
orphaning the rest inside `rx_buf`. Confirmed via temporary
`info_report()` tracing on a real boot: sending a single byte (one
`\n`) worked fine, but the very next multi-byte burst (`"x\n"`, 2
bytes in one chardev callback - exactly what a real terminal sends for
an ordinary keystroke plus Enter) left the model's RX buffer
non-empty after the kernel's ISR returned. Since this model's IRQ line
is level-triggered on "`rx_buf` non-empty" (by design, matching real
UARTDM RXSTALE semantics), the still-pending byte kept the line
asserted, which made the GIC redeliver the same interrupt immediately
on ISR exit - an infinite reentrant `msm_uart_irq()` loop, confirmed
in the trace as `MSM_UART_IMR` being rewritten hundreds of times a
second, forever, pinning the (single active) CPU at 100% and starving
every other kernel/userspace task, including whatever was supposed to
print a login prompt. This fully explains "not even Enter works" from
the user's side: their *first* keystroke likely landed fine, but
ordinary terminal behavior (Enter sending `\r\n`, key-repeat, or
simply typing a second character before the first was drained) was
near-guaranteed to deliver 2+ bytes in at least one callback shortly
after, wedging the guest solid with no further visible output at all -
indistinguishable, from a plain "did characters appear" check, from
"no console listener at all".

**Fixed**: `mr80x_uart_read()`'s RF case now pops up to 4 bytes per
32-bit read (low byte first, matching the write side's existing
packing/comment), keeping `UARTDM_RX_TOTAL_SNAP`'s reported count and
the number of bytes actually retrievable via `RF` in sync - no more
orphaned bytes, no more stuck-asserted IRQ line.

**Verified via live boots, with temporary tracing then removed
again**: (1) a trace on the RF read path showed `rx_count` draining
cleanly in one pass for a multi-byte burst (`11 -> 7 -> 3 -> 0` for an
11-byte test string) with the IRQ line deasserting immediately after,
no reentrant storm; (2) a 260+ second boot sending 21 separate Enter
presses plus a full command string produced zero crashes, zero CPU-
pinning storms, and normal continued kernel activity throughout
(`urngd` at ~150-160s, background thermal-zone timeout/disable
messages past 220s) - the hard hang is gone.

**Still open, and now properly root-caused rather than dismissed**:
even with the RX bug fixed, no shell prompt or command echo has yet
been observed. Traced one level further (read-only, `procd`'s own
source in the OpenWrt build tree, not modified): `STATE_INIT`
(`procd`'s `state.c`, the exact `"- init -"` log line already seen in
every boot) does unconditionally run `procd_inittab_run("askconsole")`
at that point, which - per `/etc/inittab`'s
`::askconsole:/usr/libexec/login.sh` - forks `/sbin/askfirst
/usr/libexec/login.sh` (`inittab.c`'s `askconsole()` always redirects
through the `askfirst` binary, by design). `askfirst`'s own source
(`utils/askfirst.c`) is trivial: `printf("Please press Enter to
activate this console.\n")`, then block in a `getchar()` loop until it
sees a `0xA` byte, then `execvp()` into the real login shell. That
banner text has never once appeared in any captured boot log,
including the long post-fix runs with plenty of real `0xA` bytes sent
- which (since this model's TX path is independently confirmed
solid, carrying thousands of lines of kernel dmesg with zero loss)
points at C stdio buffering: `printf()` is fully-buffered rather than
line-buffered unless connected to something `isatty()` recognizes as
a terminal at startup, and a full buffer that's never explicitly
`fflush()`-ed and never reached by a normal `return`/`exit` (this
program blocks forever in `getchar()`) never actually reaches the
underlying fd. Whether that's a genuine emulation gap (something about
this UART model `isatty()` needs and doesn't get - unconfirmed) or
purely userspace/musl stdio behavior unrelated to hardware is not yet
determined; investigating further means tracing into `procd`/musl
runtime behavior rather than hardware register modeling, right at the
edge of (arguably past) this project's "emulate the hardware" scope -
left here as a clearly root-caused, well-understood next step rather
than re-asserting the earlier wrong "it's just rootfs config" call.

## 33. Testing with a real, complete production rootfs (not just the initramfs recovery image) - two more real UBI-image bugs found and fixed, and a conclusive answer on why it still can't reach userspace

The user asked, reasonably, why the missing-console-prompt investigation
should be tied to one specific test image at all - "isso tem que
funcionar pra qualquer imagem que vc usar seja completa ou recovery, o
importante é emular o hardware da board" (this has to work for any
image, complete or recovery - what matters is emulating the board's
hardware). Since section 32 ended in an unresolved procd/musl mystery
on the *initramfs* image specifically, the natural next step was to
try `tools/build_full_firmware_openwrt.py` against OpenWrt's own real,
complete `squashfs-factory.ubi` build (real procd, real opkg-installed
packages, a real `/etc/inittab` - not the stripped-down initramfs
recovery/test image) instead, to see whether the very same mystery
reproduces there too, or whether it's specific to the initramfs image
after all.

**New `--openwrt-squashfs-factory` mode**: extracts this device's own
real `kernel` (UBI vol 0, a FIT) and `rootfs` (UBI vol 1, squashfs)
volumes straight out of OpenWrt's `*-squashfs-factory.ubi` build
output, repacks the kernel FIT with this project's required config
name the same way `--openwrt-fit` already did, and writes *both*
volumes into the output image (vs. the original code path, which only
ever touched the kernel volume). Needed generalizing
`patch_kernel_volume()` into a vol-agnostic `patch_volume()` (now
takes an explicit `vol_type`) and a new `extract_ubi_volume()` helper
(reassembles a volume from its own LEBs using the *full* LEB_SIZE per
LEB, not each VID header's own `data_size` field - real *dynamic*
volumes, confirmed on this device's own "kernel" volume inside
`squashfs-factory.ubi` and its "ubi_rootfs" volume in a real captured
`FULL_FIRMWARE.bin`, carry `data_size=0` unconditionally, so there is
no UBI-level way to know how many trailing bytes of the last LEB are
"real" without trusting the payload's own self-describing length - a
FIT header's, or here, squashfs's own superblock `bytes_used`).

**Two real bugs found in `patch_volume()`/`build_vid_hdr()` while
bringing this up** (both confirmed the hard way against the *real*
mainline U-Boot UBI source already vendored in this workspace's
`appsbl/vendor/u-boot-2016/drivers/mtd/ubi/`, not guessed - every
offline structural re-check this script's own `find_volume_lebs()`
could do passed cleanly *both* times, and the image still failed a
real boot with "UBI init error 22" *both* times, meaning the bug was
in something neither this tool nor its own sanity checks were looking
at at all):

1. **Shrinking a volume left stale, still-valid LEBs of its own
   *old*, larger size behind.** The real `FULL_FIRMWARE.bin`'s
   "ubi_rootfs" volume had 157 real PEBs; the new squashfs data only
   needed 89. `patch_volume()` only overwrote the first 89 of the
   existing 157 PEBs, leaving PEBs 89-156 completely untouched - still
   carrying perfectly valid VID headers claiming to be LEBs 89-156 of
   volume 1. The real UBI attach scanner
   (`drivers/mtd/ubi/attach.c`) found all 157 anyway, and
   `check_av()` (`drivers/mtd/ubi/vtbl.c`) rejects the whole image
   outright when a volume's scanned `leb_count`/`highest_lnum`
   disagrees with the volume table's freshly-shrunk `reserved_pebs`.
   First fix attempt (blanking just the VID header's 4-byte magic)
   *still* failed identically - `ubi_io_read_vid_hdr()`
   (`drivers/mtd/ubi/io.c`) only treats a bad-magic PEB as genuinely
   *empty* if `ubi_check_pattern()` finds the *entire* 64-byte header
   all-`0xFF`; anything else with a bad magic (i.e. a header with a
   corrupted magic but otherwise-stale old content, exactly what a
   4-byte-only blank produces) is instead treated as *corrupted*,
   silently added to the attach info's corrupted-PEB list. Real fix:
   blank the *whole* 64-byte VID header region to `0xFF`, not just its
   magic.
2. **Dynamic-volume LEBs need `used_ebs=0`, not the volume's real LEB
   count.** `build_vid_hdr()` already special-cased `data_size`/
   `data_crc` to 0 for dynamic volumes (matching real dynamic-volume
   LEBs observed on-flash), but kept writing `used_ebs` unconditionally
   as the volume's real LEB count either way - a field name that
   reads like it should just be "how many LEBs this volume uses",
   reasonable to assume matters for *any* volume type. It doesn't:
   `validate_vid_hdr()` (`drivers/mtd/ubi/io.c`) explicitly rejects a
   dynamic-volume LEB with a non-zero `used_ebs` ("bad used_ebs"). This
   was the fix that actually got a real attach to succeed - confirmed
   by isolating the two volumes and testing each alone: patching only
   the kernel (static) volume always worked (matches this tool's
   entire prior history, never having touched a dynamic volume
   before); patching only the rootfs (dynamic) volume alone
   reproduced "UBI init error 22" by itself, and *only* went away
   after this fix.

**Both fixes verified via isolated single-volume test boots** (kernel-
only patch: real attach succeeds, boots to "Waiting for root device"
as expected since the untouched original rootfs volume doesn't match
this newer kernel; rootfs-only patch against the *original* kernel:
real attach succeeds, that older kernel panics trying to mount the
brand new squashfs data as its OWN, incompatible rootfs layout -
expected, not a bug) **and then together** (both volumes patched at
once, matching real production use): UBI attach succeeds cleanly, no
"UBI init error 22" - the real, modern kernel boots as far as its own
in-kernel NAND driver.

**Conclusive, valuable negative result**: it still can't reach a
mounted rootfs, but *why* is now fully understood and is not a new
bug - `UBI error: cannot open mtd rootfs, error -2` /
`Waiting for root device /dev/ubiblock0_1...` are direct, expected
consequences of `qcom_snand`'s probe failure (`-110`), already found
and deliberately deferred in section 31 as needing new BAM/DMA-engine
protocol work comparable in size to the original NAND modeling effort.
u-boot/appsbl's own NAND reads (this project's actual, long-since-
working QPIC/BAM emulation) are a *completely separate* code path from
the *kernel's* own runtime NAND/UBI access once it's running - fixing
the UBI *image contents* (this section's two bugs) was necessary but
was never going to be sufficient on its own, since the modern kernel
can't read NAND *at all* yet once it's the one doing the reading. This
rules out "maybe the UBI image itself is wrong" as an explanation for
good, and pins the *entire* remaining gap on the one already-known,
already-deferred qcom_snand task - concretely, that task is now a
prerequisite for *any* real, non-initramfs rootfs to ever boot under
this emulator, not just a nice-to-have.

**Where this leaves the two open threads**: the initramfs recovery
image (section 32's still-unsolved askfirst/musl-stdio mystery)
remains the *only* currently-viable path to a running userspace at
all, since it needs no runtime NAND access - the full-production-
rootfs path validated in this section is real, working UBI-image
tooling now committed for whenever `qcom_snand` gets tackled, but
can't itself be used to cross-check the console-prompt mystery until
then.

## 34. Chasing the missing console prompt further: a dead-end tooling limitation confirmed, and a sharper (still unresolved) data point

Two more concrete steps on section 32's still-open mystery, both aimed
at getting a definitive answer rather than more theorizing:

**GDB struct introspection is dead for this exact kernel build, for a
confirmed, structural reason** - attaching `gdb-multiarch` to a live
`-s` (gdbserver) QEMU instance with `vmlinux-initramfs.debug` loaded
resolves plain symbols fine (`print &init_task` correctly gives its
real address, `whatis` correctly names its type), but `ptype
init_task`/`ptype struct task_struct` both come back
`<incomplete type>` - no member list at all, making a `for_each_
process()`-style task-list walk impossible. Root cause confirmed by
reading this exact kernel build's own `.config`, not guessed:
`CONFIG_DEBUG_INFO_REDUCED=y` - a real, deliberate OpenWrt kbuild
default that emits DWARF without full struct definitions for most
types to save build time/disk space. The kernel's own
`scripts/gdb/vmlinux-gdb.py` (present in this workspace's build tree,
read-only) checks for exactly this and refuses to load at all when
it's set ("Reduced debug information will prevent GDB from having
complete types") - so even the *proper*, Linux-aware `lx-ps` tooling
couldn't have helped here either way (separately, that tooling's own
`constants.py` was never generated for this build, which itself would
need a `make scripts_gdb` build step - out of bounds regardless). This
isn't a gap in effort; the debug information genuinely isn't in the
ELF, full stop. Closed for good, not just paused.

**A sharper, still-unresolved data point**: rather than keep chasing
*why* `askfirst`'s banner never appears, tested whether the
UART+kernel-tty stack supports *any* interactive process at all by
bypassing every layer of OpenWrt's own init logic - `rdinit=/bin/sh`
on the kernel cmdline (added via the same `--extra-bootargs`
mechanism, `tools/build_full_firmware_openwrt.py`) makes the kernel
exec bare BusyBox `ash` directly as PID 1, confirmed via
`Run /bin/sh as init process` in dmesg (no `/init`, no procd, no
`askfirst`, no `uci`, no musl-stdio-buffering concerns specific to
`printf()` - a single static binary). Sent input well after this
line, over a comfortably long window (multiple retries, up to 30s):
**zero output** - no shell prompt, no character echo, and critically,
not even the *output of a command that should run unconditionally
regardless of interactive-mode detection* (`echo
HELLO_RDINIT_WORKS` never appeared, and a real ash - interactive or
not - executes and prints output for commands read from stdin either
way, it only skips the `$ ` prompt/line-editing in non-interactive
mode). That specifically rules out "ash decided not to be
interactive" as the explanation and points at something further
upstream: either the bytes genuinely aren't reaching *this* process's
stdin (despite the hardware-level RX fix in section 32 being
independently verified correct via direct register-level tracing), or
`ash` is blocked before ever reaching its read loop for a reason
unrelated to procd/`askfirst` entirely.

**Superseded by section 35**: this "need a custom minimal init binary"
conclusion was too pessimistic. Direct UART tracing later proved RX was
already fine all the way through the kernel handler; the missing
piece was TXLEV interrupt modeling for userspace/TTY output.

## 35. MILESTONE: userspace console fixed - RX was fine, TXLEV was the missing half of the UARTDM interrupt model

Claude's section 34 left a precise but still confusing data point:
`rdinit=/bin/sh` definitely executed (`Run /bin/sh as init process`),
but no prompt, no echo, and no command output ever appeared. The first
important retest reproduced that exactly with:

```sh
cd /media/dados_2tb/opw/arm-selfmod-lab/qemu-ipq5018
./run.sh --no-net --nand-image images/full_firmware_openwrt_rdinitsh.bin
```

Sending `echo HELLO_RDINIT` produced no output before this section's
fix.

The useful breakthrough came from a temporary opt-in UART trace
(`MR80X_TRACE_UART=1`): input sent after `Run /bin/sh as init process`
arrived at the QEMU chardev callback, raised the UART IRQ, made the
Linux `msm_serial` interrupt handler read `MISR=RXSTALE`, read
`RX_TOTAL_SNAP`, and drain every byte from `UARTDM_RF` in packed
32-bit words. In other words, section 32's RX packing fix was correct:
userspace silence was **not** because bytes failed to enter the
kernel.

The actual missing half was TX interrupt modeling. Kernel `printk`
and the serial console's polling path can write directly to TF, so
dmesg looked healthy even without it. Normal TTY/userspace output is
different: line discipline echo, shell banners, prompts, and command
output are drained by `msm_serial`'s interrupt-driven TX path. That
path sets `IMR.TXLEV` and expects a UART interrupt whose `MISR`
contains `TXLEV`; without it the output stays queued forever. This is
why the emulator could show kernel logs while a real shell appeared
mute.

Fix implemented in `board/mr80x.c`:

- track the UART interrupt mask register (`IMR`) instead of ignoring
  writes to `0x14`;
- return masked RXSTALE/RXLEV/TXLEV bits from `MISR` (`0x10`);
- raise/lower the QEMU GIC line from the same masked pending condition
  the Linux driver expects;
- return `ISR_TX_READY` for read-side `ISR` (`0x14`);
- stop treating `UARTDM_NCF_TX` reads (`0x40`) as receive-FIFO reads
  - Linux reads NCF_TX back as an ordering barrier in
  `msm_reset_dm_count()`, and the old alias produced confusing
  zero-byte RF pops during TX setup;
- keep `MR80X_TRACE_UART=1` as an opt-in diagnostic for future UART
  regressions.

Verified after rebuilding `mr80x-qemu:9.1.0`:

1. `rdinit=/bin/sh` now opens a real shell:

   ```text
   Run /bin/sh as init process
   BusyBox v1.38.0 (...) built-in shell (ash)
   /bin/sh: can't access tty; job control turned off
   ~ #
   ```

   Command round-trip:

   ```text
   echo TXLEV_OK && uname -a
   TXLEV_OK
   Linux (none) 6.12.94 #0 SMP Fri Aug  7 20:47:44 2026 aarch64 GNU/Linux
   ```

2. The normal initramfs image now reaches the OpenWrt shell over the
   real APPSBL -> NAND/UBI/FIT -> AArch64 handoff path:

   ```sh
   ./run.sh --no-net --nand-image images/full_firmware_openwrt.bin
   ```

   Relevant console proof:

   ```text
   init: Console is alive
   Press the [f] key and hit [enter] to enter failsafe mode
   ...
   BusyBox v1.38.0 (...) built-in shell (ash)
   OpenWrt SNAPSHOT, r35461-10f736806d
   root@OpenWrt:~#
   ```

   Command round-trip:

   ```text
   uname -a; cat /proc/cmdline
   Linux OpenWrt 6.12.94 #0 SMP Fri Aug  7 20:47:44 2026 aarch64 GNU/Linux
   ubi.mtd=rootfs root=mtd:ubi_rootfs rootfstype=squashfs rootwait root=/dev/ubiblock0_1 coherent_pool=2M module_blacklist=vxlan,x_tables,nfnetlink,nf_nat,nf_tables,nf_conntrack,ppp_generic
   ```

Remaining noisy boot errors after this point are no longer console
blockers. The important ones are the previously-triaged runtime
hardware gaps: Linux `qcom_snand` still fails (`-110`) and blocks any
non-initramfs rootfs; MDIO/GMAC still needs better clock/PHY modeling;
WiFi remoteproc/ath11k still needs major firmware/coprocessor work;
and the current OpenWrt build tree still has stale/incompatible module
artifacts producing many `Unknown symbol` messages. The milestone here
is narrower but important: the real boot chain is now interactive.

## 36. Error tracker from the 2026-08-10 full OpenWrt interactive boot log

Source log: `/home/fabiano/.codex/attachments/6749c306-7d63-4cc6-b777-a73b637bb85b/pasted-text.txt`.

This log is important because it proves the current emulator can boot
the real chain far enough to reach an interactive OpenWrt shell:

```text
login[1023]: root login on 'console'
BusyBox v1.38.0 (...) built-in shell (ash)
OpenWrt SNAPSHOT, r35461-10f736806d
root@OpenWrt:~#
```

The remaining work is now a hardware-fidelity/error-cleanup tracker.
Status values used below:

- **open**: still visible in this log and needs work;
- **triaged**: understood, but not the next emulation blocker;
- **data/build**: likely caused by the firmware/rootfs contents or
  OpenWrt build artifacts rather than by the emulated SoC itself;
- **fixed**: corrected in this branch and should disappear from later
  comparable logs.

| ID | Status | Log evidence | Impact | Current interpretation / next action |
| --- | --- | --- | --- | --- |
| E36-01 | triaged | `ipq_spi: SPI Flash not found (bus/cs/speed/mode) = (0/0/48000000/0)` | U-Boot probes SPI and prints a failure, but continues from serial NAND. | Current OpenWrt/MR80X path is serial NAND/QPIC. Keep this tracked until we confirm from the vendor DTS/board files whether this model needs a dummy SPI NOR device or whether the warning is harmless on real hardware too. |
| E36-02 | data/build | `*** Warning - bad CRC, using default environment` | U-Boot falls back to compiled defaults instead of persisted environment. | The NAND-backed image either lacks a valid env partition/checksum at the probed location, or our env partition mapping still differs from real flash. Later fix: map/persist the real APPSBL/U-Boot env area and validate with `printenv`/`saveenv`. |
| E36-03 | open | `fdt_fixup_qpic: QPIC: unable to find node '/soc/qpic-nand@79b0000'` | U-Boot cannot patch the NAND node it expects in the Linux DT. | DT node naming/address mismatch. Audit the DTB passed inside the FIT versus U-Boot's hardcoded fixup path; either preserve/add the expected alias/node or adjust FDT patching so QPIC NAND fixups land. |
| E36-04 | open | `GIC CPU mask not found - kernel will fail to boot.` and `GICv2m: Invalid MSI base SPI (base:0)` | GIC DT is incomplete/odd; kernel survives but IRQ/MSI topology is not faithful. | Fix generated/patched interrupt-controller DT properties. MSI matters later for PCIe/WiFi/other peripherals. |
| E36-05 | open | `arch_timer: Unable to find a suitable frame in timer @ 0x...0b120000`; `Failed to initialize '/soc@0/timer@b120000': -22` | Generic timer frame node is invalid for Linux. | Either model the expected frame registers or patch/disable the bad frame node while keeping architected timer support correct. |
| E36-06 | open | `psci: failed to boot CPU1 (-22)`; `CPU1: failed to boot: -22` | Emulator boots single-core even though the DT/kernel expects SMP. | Implement enough PSCI CPU_ON handling / secondary CPU release for IPQ5018 AArch64 SMP, or patch DT to one CPU until SMP is modeled. |
| E36-07 | open | `qcom-smem 4ab00000.smem: SMEM is not initialized by SBL`; probe `error -22` | Linux cannot consume Qualcomm SMEM metadata. | U-Boot tolerated our current minimal handoff, but Linux wants a valid SMEM table/header. Build a Linux-compatible fake SMEM region from vendor/OpenWrt expectations. |
| E36-08 | triaged | `qcom_scm firmware:scm: failed to set download mode: -1` | Usually non-fatal, but the SCM emulation is incomplete. | Implement/accept the specific SCM call used for download-mode disable so Linux stops warning. Lower priority than NAND/MDIO. |
| E36-09 | open | `qcom_snand 79b0000.spi: failure in submitting spi init descriptor`; `bam-dma-engine ... Cannot free busy channel`; probe `error -110` | Biggest runtime storage blocker: Linux cannot attach the serial NAND, so a non-initramfs rootfs will not mount from real flash. | U-Boot NAND works, but Linux `qcom_snand` uses the DMA-engine/BAM path differently. Need model the Linux BAM descriptor flow, not only the U-Boot transaction path. |
| E36-10 | open | `ipq4019-mdio 88000.mdio ... error -22`; `ipq4019-mdio 90000.mdio ... error -22` | Ethernet PHY discovery in Linux cannot start. | Likely clock/reset/MDIO register model gap. Audit OpenWrt DTS clock/reset requirements and QEMU MDIO/GMAC implementation. |
| E36-11 | open | `ipq5018-gmac-dwmac ... IRQ eth_wake_irq not found`; `IRQ sfty not found`; deferred probe: `failed to parse stmmac dt parameters` | Linux GMAC does not probe. | Some IRQ names and/or stmmac DT parameters are missing in the effective DT, plus MDIO is already failing. Fix DT + clock/reset + MDIO together. |
| E36-12 | open | `genirq: Setting trigger mode 1 for irq 24 failed`; `qcom-q6-mpd ... failed to acquire wdog IRQ`; remoteproc probe `error -22` | WiFi remoteproc cannot start. | Need valid WCSS/Q6 watchdog IRQ wiring and a broader remoteproc/firmware-loading model. Not required for NAND/rootfs, but required for "100%" hardware emulation. |
| E36-13 | open | `thermal thermal_zone0..3: Temperature check failed (-110)` | Thermal zones time out. | Implement TSENS/thermal register responses or patch DT to defer thermal zones until the model exists. |
| E36-14 | open | `UBI error: cannot open mtd rootfs, error -2` | Kernel cannot find runtime MTD `rootfs`; real flash rootfs boot is blocked. | Downstream of E36-09: Linux serial NAND probe fails, so no MTD partition table appears. Fix NAND DMA-engine path first. |
| E36-15 | data/build | repeated `jbd2: Unknown symbol ...`; `xhci_hcd: Unknown symbol ...`; later `kmodloader: 5 modules could not be probed` | Module noise during early boot. | Looks like OpenWrt module/kernel ABI mismatch or intentionally incomplete initramfs module set. Track separately from SoC emulation unless reproduced with a clean matching OpenWrt build. |
| E36-16 | open | `ipq5018-tlmm ... unable to lock HW IRQ 14/16`; `gpio-keys ... failed to request irq` | Reset/WPS key IRQs do not work. | Improve TLMM GPIO direction/IRQ locking semantics so `gpio-keys` can claim button lines. |
| E36-17 | data/build | `Cannot parse config file '/etc/fw_env.config': No such file or directory` | OpenWrt cannot read U-Boot env from userspace. | Rootfs config/package issue unless we decide to ship an emulator-specific `/etc/fw_env.config` in the test image. Related to E36-02 but not an SoC blocker. |
| E36-18 | data/build | `Failed to find NVMEM device` | Board scripts cannot fetch calibration/MAC data through Linux NVMEM. | Could be downstream of NAND/ART/NVMEM DT wiring. Re-evaluate after E36-09 and ART partition exposure are fixed. |
| E36-19 | triaged | `platform cpufreq-dt`, `qcom,apss-ipq6018-clk`, `smp2p-wcss` deferred probes | Several late probes stay pending. | Expected while clock/SMEM/SMP2P/remoteproc are incomplete. Track as umbrella symptoms of E36-07/E36-12 and clock-tree work. |
| E36-20 | data/build | many networking modules: `ovpn`, `tun`, `ip_tunnel`, `nf_*`, `wireguard`, `batman_adv` `Unknown symbol`; `Module ... is blacklisted`; `kmodloader: 71 modules could not be probed` | Firewall/VPN/overlay modules do not load in this image. | Mostly OpenWrt build/package ABI hygiene, made noisier by the existing `module_blacklist=` bootargs. Not the reason the board boots or fails to mount NAND. |
| E36-21 | data/build | `refcount_t: underflow; use-after-free` in `qrtr` while loading modules | Kernel warning during QRTR module init. | Likely triggered by module/rootfs mismatch or QRTR running without the expected remoteproc/QRTR peers. Recheck after E36-12 and module ABI cleanup. |
| E36-22 | open | `ath11k c000000.wifi: failed to get rproc: -517`; `ath11k b00a040.wifi: failed to get rproc: -517` | WiFi cannot start. | Downstream of remoteproc/SMEM/SMP2P model gaps. Required for full router emulation, but behind NAND + Ethernet in priority. |
| E36-23 | fixed | No missing userspace prompt in this log; shell reaches `root@OpenWrt:~#`. | Confirms the previous UART userspace-console blocker is gone. | Fixed by section 35 (`TXLEV`/`MISR`/`IMR` UARTDM interrupt modeling). Keep as a regression check in future logs. |

Priority order from this log:

1. **E36-09 / E36-14: Linux serial NAND through BAM DMA** - this is
   the reason a normal non-initramfs rootfs still cannot come from the
   full flash image at runtime.
2. **E36-10 / E36-11: MDIO + GMAC** - next required piece for a useful
   router boot after storage.
3. **E36-03 / E36-04 / E36-05 / E36-07: effective DT and Qualcomm
   handoff data** - these are early correctness problems that will
   affect multiple drivers.
4. **E36-12 / E36-22: remoteproc + WiFi** - large follow-up for full
   hardware fidelity.
5. **E36-15 / E36-20 / E36-21: OpenWrt module ABI cleanup** - useful
   for a clean log, but separate from QEMU hardware modeling.
