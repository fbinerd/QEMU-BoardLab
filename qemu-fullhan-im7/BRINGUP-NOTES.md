# Fullhan `im7` (Imou IPC-S21F) QEMU machine — bringup notes

Separate board from `qemu-ipq5018/` (MR80X/IPQ5018). Different vendor,
different SoC family, different research question. Kept in its own
directory, own Kconfig symbol (`CONFIG_IM7CAM`), own Dockerfile/vendor QEMU
copy, own `-M` machine name — nothing here touches `qemu-ipq5018/board/mr80x.c`
or its build. This file is this board's own running log, separate from
`qemu-ipq5018/BRINGUP-NOTES.md` on purpose (per session instruction — don't
mix histories).

## Where the firmware came from

Not this repo's own research — the flash dump and all partition analysis
happened in the sibling `openwrt-build-tools` repo:

```
/media/dados_2tb/opw/openwrt-build-tools/tools/firmware-lab/work/miboim7-tudo-sobre/
```

That folder (its own independent git repo) has the full writeup: device
identification (Dahua/Imou camera, model `IPC-S21F`, devalias `iM7-FC`,
rebranded by Intelbras in Brazil), the 7-way partition split
(`particoes/0_U-Boot.bin` … `6_backup.bin`), the extracted rootfs, U-Boot
environment (baud rate, TFTP recovery commands, etc.), and the RSA/cert
findings. This file only covers what's specific to making it run in QEMU —
see that repo for the device-level investigation.

Bringing the actual firmware image files into *this* repo (mirroring how
`qemu-ipq5018/images/` holds `FULL_FIRMWARE.bin`, gitignored) is TODO —
right now this board's Dockerfile/run.sh will need to point at the path
above directly, or a copy needs to land in `qemu-fullhan-im7/images/`
(already gitignored, added preemptively to this board's own `.gitignore`
pattern, same as ipq5018's).

## Unlike MR80X: no vendor GPL source available

The MR80X/appsbl board (`qemu-ipq5018/board/mr80x.c`) was built entirely
from real Qualcomm/Mercusys vendor GPL source — every register address in
that file is a transcription, never a guess (see that file's own header
comment). We have no equivalent for this device: Fullhan doesn't publish
public datasheets, and no GPL source drop for this exact firmware build
was found. Everything below comes from two sources instead:

1. **Disassembling our own extracted U-Boot binary** (`0_U-Boot.bin`,
   320 KB, U-Boot 2010.06 banner, built 2024-09-27) — using
   `arm-none-eabi-objdump`/`capstone` (this repo's own root `Dockerfile`
   already has `binutils-arm-none-eabi`, reused for this).
2. **Independent third-party reverse-engineering of a *sibling* Fullhan
   chip** (FH8852V201, a different but same-vendor-family SoC), from the
   public repo
   [`pavliha/fh8852v201-dump`](https://github.com/pavliha/fh8852v201-dump)
   (part of the [OpenIPC](https://openipc.org/cameras/vendors/fullhan)
   community project, which maintains open firmware for several Fullhan
   SoCs). Their Ghidra-reversed register map lists `UART0 @ 0xF0700000`,
   `I2C0 @ 0xF0200000`, `GPIO0 @ 0xF0300000`, `SPI0 @ 0xF0500000` for that
   chip. **Not our exact chip** — treat as a same-family hint, not a
   confirmed fact, until cross-checked against our own binary (which is
   exactly what section 2 below does).

## 1. Image format (confirmed by direct inspection)

`0_U-Boot.bin` is **not** a standard `mkimage`/ELF U-Boot image. Layout,
byte-verified:

- `0x000–0x01F`: small header. Magic `"2BL*OABD"` at offset 0, then three
  `u32` fields (`1,1,1`), then `0x00000180` (384 decimal — doesn't map
  cleanly to anything found so far), then magic `aa5555aa`.
- `0x020–0x0FF`: zeroed, then `0xFF`-padded.
- `0x100–0x17F`: a second header. Magic `"W975"` at 0x100 (likely a
  board/chip code — unconfirmed), literal string `"uboot"` at 0x140, and
  at 0x160–0x17B a field pattern that repeats the 32-bit value
  `0xA0800000` twice (bytes `00 00 80 a0` at 0x168 and 0x170) — read as a
  load-address-and-entry-point pair. **This is the strongest evidence for
  the code's RAM load base being `0xA0800000`** (see also section 2, where
  independent pointer literals inside the code cluster in the
  `0xA080xxxx`–`0xA083xxxx` range, consistent with this base plus normal
  `.bss`/heap growth).
- `0x180–0xFFF`: `0xFF` padding — confirmed by scanning to the byte, ends
  exactly at `0x1000`.
- `0x1000–~0x2000`: a table of 16-byte entries `(addr, value, 0, type)`,
  `addr` starting at `0xed000000` and incrementing by 4 per entry for the
  first ~120 entries, then continuing with non-sequential `addr` values
  and varying `value`/`type` fields. Read as a **position-independent
  relocation table** (a fixup list applied at load time, letting the image
  be relocated to whatever address it's actually loaded at without every
  reference needing to be link-time-fixed) — consistent with real ARM code
  in this image using PC-relative `ADD/SUB pc, #imm` (ADR-style) addressing
  for local string references instead of absolute literal-pool loads (see
  section 2 — that's *why* the literal-pool search came up empty at first).
  Exact table length/end not pinned down (variable-length, not worth fully
  parsing for this purpose — the relevant addresses were found from the
  *code*, not from decoding this table).
- **`~0x2000` onward: real ARM code.** Found empirically, not from any
  header field: scanned the whole 320 KB for the byte pattern matching
  `STMFD sp!, {…, lr}` (`push {…, lr}`, the standard AAPCS function
  prologue — file bytes `?? ?? 2d e9`, word-aligned) and bucketed hits per
  4 KB window. Zero hits below `0x2000`; dense, roughly uniform hits from
  `0x2000` through `~0x28000`; then it thins out and a string table
  (containing `"U-Boot 2010.06 (Sep 27 2024 - 18:56:13)"` at `0x29718`,
  confirmed earlier in the device-level investigation) starts around
  `0x29000+`. Standard `.text` → `.rodata` layout.
- **Open question**: does the loader that reads this partition copy the
  *whole* 320 KB (header included) to `0xA0800000`, or does it skip the
  header/reloc-table and place file offset `0x1000` (or `0x2000`) at
  `0xA0800000`? Not resolved yet — matters for getting the QEMU `-kernel`
  load address exactly right. Current board (section 3) guesses "whole
  file at `0xA0800000`" since that's what the header's own load-address
  field most naturally implies, and is the cheapest to try first.

## 2. UART base address: `0xF0700000` (medium-high confidence)

Cross-validated two independent ways:

1. **Byte search of our own binary**: the 32-bit little-endian word
   `0xF0700000` appears in `0_U-Boot.bin` at file offset `0xaae8` — inside
   the dense code region identified in section 1, not in the header or
   reloc table.
2. **Disassembly at that exact site** (`arm-none-eabi-objdump
   --start-address=0xaa00 --stop-address=0xab80`, ARM mode, raw binary — no
   base address needed since this is purely a local literal-pool read):

   ```
   aab8: ldr r1, [pc, #40]  @ 0xaae8   -> r1 = 0xF0700000
   aabc: ldr r0, [pc, #40]  @ 0xaaec   -> r0 = 0xA083E4A0
   aac0: push {r4, lr}
   aac4: mov r2, #9
   aac8: mov r3, #7
   aacc: mov ip, #65        @ 0x41 = 'A'
   aad0: stm r0, {r1, r2, r3, ip}      -> writes 4 words starting at 0xA083E4A0:
                                           [0]=0xF0700000 [1]=9 [2]=7 [3]='A'
   aad4: bl 0xaa5c                     -> some init call, taking the struct
   aad8: ldr r3, [pc, #16]  @ 0xaaf0   -> r3 = 0xA082DDD8
   aadc: mov r0, #0
   aae0: str r0, [r3]                  -> zero something else
   aae4: pop {r4, pc}
   ```

   Reads exactly like a device/driver registration call: populate a struct
   with `{base_addr, ?, ?, tag_or_flags}` and hand it to an init function.
   `0xA083E4A0`/`0xA082DDD8` both fall in the `0xA080xxxx`–`0xA083xxxx`
   range, consistent with the `0xA0800000` RAM base from section 1 (these
   read as this image's own `.bss`/static-variable addresses, further
   corroborating that base).

   Fields `9`, `7`, `'A'` are **not yet explained** — could be
   IRQ number / FIFO depth / port index / a tag byte, or something else
   entirely; not enough to say. This call might not even be UART-specific
   (could be a generic "register a char device" helper also used for
   something else) — flagging that as a real possibility, not just
   covering myself.

3. **Cross-check against `fh8852v201-dump`'s independently reverse-engineered
   `UART0 @ 0xF0700000`** for a sibling Fullhan chip (different exact SoC,
   same vendor/family) — matches exactly.

**Net assessment**: strong enough to build the first board iteration around
(better than a guess — same address found two independent ways, one of them
from our own binary), but the exact in-block register layout (TX data
offset, TX-ready/status bit position, RX offset, baud/config registers) is
**not** confirmed. Section 3's board therefore starts with a trace/stub
peripheral at this address rather than a fully modeled UART — the plan is
to run real code against it and read the access pattern back out (offsets
touched, values written, in what order) to reverse the actual protocol
empirically, the same bootstrapping trick implicit in how `qemu-ipq5018`
got its first UART bytes out before it had a complete driver
cross-reference.

## 3. CPU / ISA: unconfirmed, placeholder chosen

No direct evidence yet for which ARM core this is. What we do know:

- Code disassembles cleanly as standard 32-bit **ARM** (not Thumb) at every
  point sampled so far — no `UNDEFINED` instructions inside real `.text`
  regions (the only `UNDEFINED` hits were inside data/reloc-table regions
  being mis-disassembled as if they were code, expected and harmless).
- Kernel is Linux 4.9.129, `zImage`, plain ARM (not arm64) per the uImage
  header in `3_Kernel.bin` — so this is definitely a 32-bit-only part,
  unlike MR80X's AArch64 surprise.
- Low-end Fullhan IPC SoCs of this era commonly use ARM926EJ-S (ARMv5TE) or
  Cortex-A5/A7 depending on tier — genuinely don't know which for this
  chip without more evidence (no `VFP`/NEON instructions spotted in the
  small samples disassembled so far, which would rule ARM926 *in*, but
  that's a thin sample, not a real survey).

**Placeholder for the first board iteration: `arm926`** (QEMU's
`arm926`) — cheapest/most conservative pick for an old-ish embedded SoC of
this class, and if wrong, running the real kernel/U-Boot against it should
surface `UNDEFINED` instruction faults quickly (unlike a wrong UART address,
which just silently produces no output) — a much faster wrong-guess signal
to iterate on than the UART question. Revisit if/when execution gets far
enough to hit real ARMv7-only instructions.

## 4. First boot attempts (empirical, all this session)

Board built (`docker build`, `arm-softmmu`/`qemu-system-arm`, compiles
clean) and run against the real `0_U-Boot.bin` three times, each time
changing exactly one variable based on what the previous run showed —
same iterative method as `qemu-ipq5018`'s own history, just three cycles
of it so far instead of dozens.

**Attempt 1** — whole 320 KB file loaded at `0xA0800000`, entry PC =
`0xA0800000` (i.e., the raw load base, no offset). Ran 10s: only the
"loaded ... at 0xa0800000" info line, then nothing — no UART trace, no
crash. Read as: the CPU started executing the image's *header* bytes
(`"2BL*OABD"` etc., not real code — see section 1) as garbage ARM
instructions, almost certainly hit an undefined-instruction trap almost
immediately, and got stuck looping at the (unmapped, reads-as-zero,
decodes as `andeq r0,r0,r0`) default vector base — silently, forever,
never reaching real code or the UART stub at all.

**Attempt 2** — same load, entry PC = `0xA0800000 + 0x2000` (section 1's
independently-found real-code start offset). Ran 10s, output:

```
qemu-system-arm: info: im7cam: loaded '/fw/0_U-Boot.bin' (327680 bytes) at 0xa0800000
write access to unsupported AArch32 system register cp:15 opc1:0 crn:12 crm:0 opc2:0 (non-secure)
```

That coprocessor register (cp15, c12, c0, opc2=0) is **VBAR** — the
exception vector base register. It's a real ARMv7-A (or ARMv6+Security-
Extensions) register that **does not exist at all** on ARMv5 — `arm926`
(the placeholder CPU picked in section 3, before this evidence existed)
correctly refused it. Two things confirmed by this single line: (a) the
`0x2000` entry offset is right, or at least right enough to reach genuine
early boot code — a wrong offset landing back in garbage would not have
produced a coherent, real, meaningful instruction like a VBAR write; (b)
the real CPU is ARMv7-A-class, not ARMv5 — `arm926` was wrong.

**Attempt 3** — same as attempt 2, CPU switched to `cortex-a7`. Ran 10s:
only the "loaded..." line again, no warnings, no crash, no UART/peripheral
trace. VBAR write now presumably succeeds silently (cortex-a7 supports it).
Execution is getting further than attempt 1 (no early trap-loop symptom)
but hasn't reached any of the seven `0xF0??0000`-family stub regions
(UART/I2C0/GPIO0/SPI0/three unlabeled) within 10s, and
`ignore_memory_transaction_failures = true` means a genuinely unmapped
access elsewhere would be completely silent too — no way yet to tell
"looping in unmapped memory" apart from "looping in a slow real
computation" apart from "waiting on a register we do stub but haven't
logged yet because it hasn't been reached" from the current log alone.

## 5. Two real bugs found via gdbstub, then: the real U-Boot banner

Continuing straight from attempt 3's silence, live, with the user
watching (`./run.sh` from their own terminal, no output - same as
attempt 3 above).

**Diagnosis, not another guess**: attached `gdb-multiarch` to QEMU's
`-S -gdb tcp::1234` gdbstub (both containers on `--network host` so they
can reach each other), let it `continue` for a few seconds, then
snapshotted registers. `sp = 0x0`, `pc` in some address this file never
computed on purpose (`0xa5ec0270`, later `0xa2f4cc00` on a repeat) - a
stack that was never really initialized, and a CPU that had wandered off
somewhere.

**Bug 1 - RAM didn't cover the real stack address.** Disassembling
straight through from file offset `0x1000` (past the `0xFF` padding) to
`0x2100` found something the earlier push-`{lr}`-density scan (section 1)
never revealed because vector tables don't have that pattern: a genuine,
textbook ARM exception vector table sits at file offset `0x2000` -
`b 0x2054` (reset), seven `ldr pc, [pc, #20]` handler slots, and the
classic `0x12345678` vector-table magic number right after. Reset code at
`0x2054` is an equally textbook U-Boot `start.S`: CPSR mode switch to
SVC, a VBAR write (see attempt 2 above - this is the exact instruction
that faulted on `arm926`), cache/TLB invalidate, SCTLR read-modify-write
(MMU/cache enable), then `sp = *(0x2398) - 0x480000 - 0x80`. Reading
`0x2398` directly from the file: `0xA0800000` - a plain, sensible,
already-correct absolute address, so this isn't a relocation problem.
`0xA0800000 - 0x480080 = 0xA037FF80` - **~4.5 MB *below*
`IM7CAM_RAM_BASE`**, which back then was the same as the image's load
address. Every stack push landed in unmapped memory, silently discarded
(`ignore_memory_transaction_failures = true`), corrupting execution with
no error until the CPU eventually jumped somewhere nonsensical. Fixed by
separating "where the image loads" (`IM7CAM_IMAGE_LOAD_ADDR`, unchanged,
`0xA0800000`) from "where RAM starts" (`IM7CAM_RAM_BASE`, moved down to
`0xA0000000` - a round, conservative choice comfortably below the
computed SP, not itself independently confirmed as the chip's real DRAM
base).

**Bug 2 - the whole file (header included) was being loaded, but
shouldn't be.** Same diagnostic technique, after bug 1's fix: `sp` *still*
read back as `0`. The VBAR write above sets VBAR to the plain value at
file offset `0x2398`, which is `0xA0800000` - i.e., in the image's own
linked assumptions, its exception vector table's real runtime address
*is* `IM7CAM_IMAGE_LOAD_ADDR` itself, not `IM7CAM_IMAGE_LOAD_ADDR + 0x2000`
where this skeleton had been putting it (matching where the vector table
sits *in the file*, under the "load the whole file unmodified" hypothesis
section 1 started with). The only way both facts are true together is if
the real loader **skips the first `0x2000` bytes** (header + relocation
table) and places file offset `0x2000` at `IM7CAM_IMAGE_LOAD_ADDR`. Fixed:
`im7cam_init()` now reads the file itself (`g_file_get_contents` +
`rom_add_blob_fixed`, since `load_image_targphys()` has no byte-offset
support) and loads everything from file offset `IM7CAM_HEADER_SKIP`
(`0x2000`) onward; the entry point is now the plain load address, no
`+0x2000` needed since that offset is now baked into what gets skipped
before loading, not added after.

**Result**: with both fixed, real trace output appeared - `0xF0700000`
(the UART address from section 2) got hit with exactly the sequence
predicted there (poll offset `0x7c` for bit `0x2`, write data to offset
`0x0`, including the literal byte `0x41`/`'A'` matching the struct-init
disassembly). Rewired the UART from a logging stub to a real (if
minimal) polled device - TX-only, status register hardcoded
always-ready, wired to the real `-serial stdio` backend the same way
`qemu-ipq5018/board/mr80x.c`'s own UART does (`qemu_chr_fe_init(...,
serial_hd(0), ...)`) - and:

```
U-Boot 2010.06 (Sep 27 2024 - 18:56:13)
```

**printed for real**, on the actual emulated console - the same banner
string this device's real hardware prints, confirmed identical earlier
in the device-level investigation (`particoes/0_U-Boot.bin`'s own
strings). First real output from this machine model.

(One more small bug hit and fixed along the way, not evidence-driven,
just a build mistake: the first UART-wiring attempt segfaulted QEMU
itself immediately, `gdb`'s backtrace pointed at `qemu_chr_fe_init`
receiving a garbage `Chardev *` - `serial_hd()` is declared in
`sysemu/sysemu.h`, which this file wasn't including, so the compiler
silently assumed an `int`-returning implicit declaration and truncated
the real pointer. Added the include, gone.)

After the banner, execution spins hard (millions of iterations within
the run) on `0xf0e00000` offset `0x1c` (write, an incrementing byte
pattern `0x00..0xff` then `0xff << 20`) then offset `0x28` (read-only
poll) - reads exactly like a timer/counter reload-then-wait-for-expiry
sequence. Made that one read return `0xFFFFFFFF` (a quick "does this
unblock it" probe, *not* a modeled register - unlike the UART case nothing
in our own disassembly has been cross-referenced for this one yet) and
the hard spin is gone: execution now walks through a long, non-repeating
sequence of distinct register writes across that same `0xf0e00000` block
(offsets `0x0`, `0x8`, `0x10`, `0x14`, `0x1c`, `0x20`, `0x2c`, `0x60`,
`0xf4`, `0xf8`, `0xfc`, `0x100`, `0x104`, ...) with values like
`0xb0000000` and `0xff00000` showing up - reads like a real clock/PLL
controller init sequence, not a stuck loop. Still running (not yet
reached another logged milestone) when the 10s test window ended.

## Status

**Real progress, not just a skeleton anymore.** The actual
`U-Boot 2010.06 (Sep 27 2024 - 18:56:13)` banner - this exact device's
real banner - prints on the emulated console. Every address load-bearing
enough to have actually mattered so far (image load/header-skip, entry
point, UART base + TX/status register offsets, CPU ISA class) is now
either directly confirmed by real execution or has real supporting
evidence, not a guess, matching this project's standard throughout.

What's proven by the fact that this booted this far:
- `IM7CAM_IMAGE_LOAD_ADDR = 0xA0800000` with the first `0x2000` bytes of
  the file skipped - confirmed (VBAR self-consistency, section 5).
- `IM7CAM_ENTRY` = that same address, no offset - confirmed.
- `cortex-a7` (or at least something ARMv7-A-class with the same visible
  behavior) - confirmed enough to reach real C code and drive a real
  peripheral correctly.
- UART at `0xF0700000`, TX data at `+0x0`, status at `+0x7c` with ready
  bit `0x2` - confirmed by the banner actually printing.
- `IM7CAM_RAM_BASE = 0xA0000000` / `IM7CAM_RAM_SIZE = 64 MiB` - good
  enough to cover everything touched so far; still not independently
  confirmed as the chip's real DRAM window.

Next steps, in order of how cheap they are to try:

1. **Let it run longer** (the 10s test window may simply not have been
   enough for a real clock/PLL init sequence + whatever comes after) -
   the cheapest thing to try before writing any more code.
2. **Model `0xf0e00000` for real** if it turns out to still be stuck
   somewhere in there on a longer run - same trace-driven method as the
   UART: find the *specific* offset it spins on next, cross-reference
   against the disassembly around where these writes originate (not done
   yet for this block - the UART case had a full disassembly
   cross-reference, section 2/4/5; this one so far is trace-only).
3. **RX path** - nothing's needed it yet (TX-only U-Boot banner output),
   but interactive console input (autoboot interrupt, `run dk`, etc. -
   see the device-level investigation's TFTP recovery notes) will need
   it. Not modeled at all currently.
4. Once past whatever `0xf0e00000` is: watch for the next stub region
   getting hit (I2C0/GPIO0/SPI0, or a genuinely new untraced address if
   `ignore_memory_transaction_failures` is masking one) and repeat the
   same method.

## 6. Real SPI controller, a real timer, and real flash-backed reads

Prompted by the user explicitly asking for NAND/flash-backed booting like
`qemu-ipq5018`'s `--nand-image`, and by two more real hangs found the same
way as section 5 (gdbstub snapshot, not guessed).

**What "im7cam.unk-0xf0e00000" turned out to be**: not one mystery block -
disassembling around the exact hang address (file offset `0xa140`, found
via `arm-none-eabi-objdump --start-address=0xa100 --stop-address=0xa200`)
showed a completely ordinary poll: `ldr r3,[r4,#0x28]; and r3,r3,#5; cmp
r3,#4; bne back`, sitting right next to the exact byte write the trace log
already showed (`off=0x60 val=0x9f`, matching `str r1,[r4,#0x60]` a few
lines up in the same disassembly). Renamed the region `IM7CAM_SPI_BASE`
and built a real (if still evidence-light past these two registers)
device: FIFO at `+0x60`, status at `+0x28` hardcoded to `0x4` (idle, no
error - satisfies that exact mask/compare). This is a *second*,
chip-specific SPI address, different from `IM7CAM_SPI0_LABELED_BASE`
(`0xF0500000`, the sibling-chip label from section 2/`fh8852v201-dump`) -
that stub is kept mapped but unused/renamed
`im7cam.spi0-guess-unused` for now, since nothing has touched it.

**The earlier "fixed" `0x28` poll broke this one**: the section 5 probe
(`return 0xFFFFFFFF` for that offset) satisfied whatever bit test came
first, but `0xFFFFFFFF & 5` is `5`, never `4` - so *this* poll, hit later
on a subsequent call into the same routine, spun forever. Same register,
two different call sites checking different exact values - a static
"always return X" guess for a shared status register was always going to
break one of them eventually. Fixed by returning the disassembly-derived
`0x4` unconditionally instead.

**A second, separate hang, found the same way (gdbstub snapshot →
disassemble the frozen PC)**: past the SPI status fix, PC froze at RAM
address `0xa0820924` (file offset `0x22924`) across two 3-second-apart
snapshots. That's inside a generic elapsed-time/timeout helper -
`ldr r2,[r0,#4]` where `r0` is a pool-constant pointer to
`IM7CAM_TIMER_BASE` (`0xF0C00000` - the same block that got written
`0x5f5e100` = 100,000,000, a very clock-rate-shaped number, during the
earlier clock/PLL-looking init sequence in section 5) - a real hardware
free-running counter read, always 0 from the old catch-all stub, so a
bounded wait-with-timeout loop's timeout side could never fire. Not an
infinite loop in the code, an infinite *wait* the skeleton was
accidentally forcing by never advancing time. Fixed with a genuinely
minimal timer device: reads at `+0x4` return `qemu_clock_get_ns
(QEMU_CLOCK_VIRTUAL)` truncated to 32 bits - always moving, not modeling
the real tick rate/width/IRQs at all.

**Result of both fixes**: the real U-Boot banner is followed by real,
device-specific output that matches the actual investigation in the
sibling `openwrt-build-tools` repo *exactly* -
`fail to load bootargsParametersV22.txt` / `bootargsParametersV21.txt`
(the literal filenames inside `2_partition.bin`'s CramFS), `DRAM: 64 MiB`,
`TEXT_BASE:a0800000`, and eventually `Net:` / `MAC: 00:12:34:56:78:9a`
before the test window ends - a large stretch of genuine, chip-specific
boot log, not a stub artifact.

**Real flash backing, `--nand-image`-style**: `im7cam_init()` now takes
`IM7CAM_SPI_IMAGE` (env var, mirroring `MR80X_NAND_IMAGE`'s pattern) and
loads the real 8 MB dump (`miboim7-spi-en25qh64-8mb-20260819.bin` from the
device-level investigation) via `g_file_get_contents()`. The SPI device
parses standard SPI NOR opcodes shifted through the FIFO: `0x03`/`0x0B`
(READ/FAST_READ) latch a 24-bit big-endian address from the next 3 bytes
and serve real subsequent bytes from the backing file, auto-incrementing;
`0x9F` (RDID) and `0x05` (RDSR) return canned responses (real EN25QH64
JEDEC ID `1C 70 17` - from general knowledge of the part, not
independently re-verified against a datasheet this session; and an
"idle, no error" status byte). A write of `0` to `+0x08` resets the
per-transaction byte accumulator - inferred from the disassembled
open/close bracket around each burst (`[r4+8]=0` before, `=1` after,
file offset `0xa120`/`0xa130`), not independently confirmed as literally
meaning "reset the FIFO parser."

**Known-bad, not yet resolved**: the flash *does* get read for real once
a `0x03`/`0x0B` command is recognized (confirmed - see next paragraph),
but the very first thing U-Boot does with it - `SF: Unsupported
manufacturer b0 e4 83 a0 00` / `Fail probe spi flash.` - still fails, and
the same exact garbage bytes print with or without the RDID/RDSR support
above. Traced (`"len is %d"`/`"Unsupported manufacturer"` string
cross-reference, file offset `0x9d78`'s function) to a *plain 32-bit*
`str r3,[r4,#0x60]` writing `0xFFFFFFFF` (`mvn r3,#0`) as what should be
the RDID trigger - added size-aware read/write to the FIFO (a real 4-byte
`ldr`/`str` should shift 4 sequential bytes, not repeat/zero-pad one) on
the theory that access-width blindness was the bug, but the *exact same*
garbage bytes came back afterward, unchanged to the byte - meaning this
particular probe path isn't exercising `IM7CAM_SPI_BASE`'s FIFO logic at
all, despite `r4` for the *surrounding* code (offsets `0x0`/`0x4`/`0x8`
etc., all logged against `im7cam.spi`) resolving correctly. Most likely
explanation not yet checked: `r0` going into that `0x9d78` function may be
a generic "spi_slave"-style struct with an *extra* level of pointer
indirection this trace hasn't been walked through yet, so the real
opcode/response exchange for *this specific call* happens against a
struct field this file hasn't identified, not against `IM7CAM_SPI_BASE`
directly. Left as-is rather than guessed further - matches the earlier
finding (section 5) that this device's own disassembly, not another
probe, is what actually resolves these.

**Doesn't block boot**: U-Boot treats the flash-probe failure as
non-fatal (`Can not find any available flash.` × N, `bad CRC, using
default environment`) and keeps going - reaches `Net:`/MAC printing
before the test window ends. Getting a real kernel/rootfs load working
still needs the RDID mismatch above resolved (or at minimum, confirming
whether the READ-path address latching this session *did* build correctly
works for a plain `sf read`/`bootm` attempt even with the ID check
failing - not tried yet).

## Status (updated)

Boots to real, device-specific U-Boot output well past the banner - DRAM
size, environment/partition-table load attempts (with real, matching
filenames), network MAC - backed by a real SPI flash device with an
actual working timer, not stubs, for everything confirmed so far. The
JEDEC ID probe is the current known-wrong piece; next session should
start there (walk `0x9d78`'s caller to find what `r0` really points to,
rather than extending the current `IM7CAM_SPI_BASE` model further on a
guess).

## 7. Physical board inventory (external research) - what's relevant here

The user had someone do a physical inspection of the real board (chip
markings, no photos shared with this file yet) and shared a component
table. Cross-referenced each item against public sources and against
what this board actually needs. Full table, for the record:

| Component | Marking read | Relevance to this QEMU board |
|---|---|---|
| Main SoC | Fullhan FH8623V100 / "UKR466-1" | High - see below |
| SPI NOR | cFeon QH64A-104HIP / EN25QH64A | **Already modeled** (section 6) |
| SPI NOR capacity | 64 Mbit / 8 MiB | Already confirmed (device-level investigation) |
| Wi-Fi | iComm/SSV SV6155P, USB-attached | Low for now - see below |
| Motor driver | marked ULN2008 | None for U-Boot/kernel bringup |
| PAN/TILT motors, IR, Ethernet port | present | None for U-Boot/kernel bringup |
| CMOS sensor | unidentified | None for U-Boot/kernel bringup |
| RAM | unidentified chip | **Already confirmed indirectly** (section 6/this section) |
| Ethernet PHY | unidentified | Matters only once real network I/O is attempted |
| UART | assumed present | **Already confirmed** (sections 2/4/5 - it's how this board boots at all) |

**FH8623V100 SoC**: searched fullhan.com's own product listings, the
OpenIPC Fullhan SoC list (`openipc.org/cameras/vendors/fullhan` - covers
FH8626/8632/8652/8833/8852/8856/8858, several generations), and
`github.com/pingumacpenguin/FH86XX_Cameras` (FH8616-specific, explicitly
warns other "FH86XX-looking" cameras use different, incompatible SoCs
underneath). **FH8623 doesn't appear in any of these** - no independent
public register map, datasheet, or GPL source drop found. Not surprising
given how fragmented/OEM-specific this vendor's part numbering is (the
device-level investigation already found the *same firmware image*
serves a whole family of differently-branded/differently-optioned camera
models - see that repo's README `bootargsParametersV2.txt` findings).
Practical effect on this file: **no change** - this board's approach has
never depended on an FH8623-specific datasheet existing; every address
modeled so far (sections 1-6) came from disassembling *this exact
device's own* U-Boot binary, cross-checked where possible against a
different Fullhan chip's independent RE (`fh8852v201-dump`) rather than
official FH8623 documentation. That remains the only reliable method -
confirmed again this session (the UART address match held, the RDID
mismatch section 6 flagged did *not* resolve by assuming a shared FH86xx
convention, consistent with "trust this device's own disassembly over
family-wide assumptions").

**RAM**: no chip marking identified by the physical inspection either,
but section 6 (above, in this same file) already independently confirmed
64 MiB is correct - not from this external research, from watching this
board's own U-Boot print a real `gd`/`bd`-struct-sourced value during an
actual boot. Cross-referencing external hardware research would only
matter here if it *contradicted* that - it doesn't (the table above
lists RAM as "unidentified" from the physical side, no conflict).

**SPI flash / JEDEC ID**: the physical marking (`cFeon QH64A-104HIP`)
and the datasheet-confirmed manufacturer ID (`0x1C`) match exactly what
`IM7CAM_SPI_JEDEC_ID` already used, itself independently derived from
flashrom's own database entry (see the updated comment on that array in
`board/im7cam.c`) rather than this external research - two independent
confirmations of the same three bytes now (flashrom's real hardware
identification during the physical dump, and this session's datasheet/
flashchips.h cross-check). No code change needed, just corroboration.

**Why the rest (Wi-Fi, motors, sensor, Ethernet PHY) isn't being modeled
right now**: all of it lives past where this board currently gets stuck
(section 6's SPI/JEDEC gap, blocking a real kernel/rootfs load) or past
where U-Boot even runs code for it at all:
- **CMOS sensor / motors / IR-cut relay**: exclusively Linux-userspace
  concerns (ISP driver, GPIO/PWM userspace control) - completely
  unreachable until a kernel actually boots, which needs the SPI gap
  closed first. Zero U-Boot-stage relevance.
- **Wi-Fi (SV6155P)**: USB-attached per the physical inspection - would
  need a modeled USB host controller *and* a full USB device model for
  the SV6155P itself before it could matter at all, and U-Boot on this
  device doesn't appear to touch Wi-Fi (only the wired `Net:`/MAC path
  logged so far, section 6) - Linux-stage-or-later, and a substantially
  bigger undertaking than anything modeled so far when it does come up.
  Noted for later: the user's own research found a public report of an
  OpenIPC SSV615x-package Imou/SigmaStar camera hitting a Wi-Fi *auth*
  failure specifically - worth remembering as a likely real obstacle
  whenever this board gets that far, not something to pre-solve now.
- **Ethernet PHY**: the one item on this list that's plausibly *soon*
  relevant, not just eventually - U-Boot already reaches `Net:`/prints a
  MAC (section 6) using this board's real Ethernet GMAC/MDIO path
  somehow, without this file modeling a PHY at all (every GMAC/MDIO
  MMIO access is presumably landing on `ignore_memory_transaction_
  failures`-silenced unmapped space or an already-mapped stub that
  hasn't been specifically checked yet). Worth a real trace-driven look
  (same method as every other peripheral in this file) *if/when* the
  next goal becomes "get a TFTP transfer working," matching the
  device-level investigation's own documented TFTP recovery mechanism -
  not blocking right now since nothing yet requires actual network I/O
  to succeed.

**Net effect on this session's code**: two comments updated (JEDEC ID
and RAM size, both in `board/im7cam.c`) to cite the now-independently-
confirmed sources instead of "not verified"/"just a placeholder" hedges.
No behavioral change - both values were already correct, this just
upgrades the paper trail behind them, consistent with this file's
standing rule of citing evidence over asserting confidence.

## 8. Reset controller, real bidirectional UART, and a stubborn RDID mystery

Prompted directly by the user hitting a real, practical problem: `-d
unimp` left on by default flooded an interactive terminal with hundreds
of log lines per retry loop (harmless, self-resolving loops - section 6
- but unusable to watch live), reading as a hang even though the
emulator was fine. Fixed first (`run.sh`): `-d unimp` is no longer
on by default, moved behind `--trace`. Confirmed the actual boot is fast
and clean without it - same 34-line log, real DRAM/env/Net output,
reached in well under the old 20s test window instead of scrolling
forever.

**A third real hang, past where testing had gone before**: with tracing
off (so testing could run longer without terminal-flood distorting
timing), the boot reliably stopped dead after `MAC: 00:12:34:56:78:9a` -
30s, zero further output, not just slow. Same method as every hang
before it: gdbstub snapshot (`0xa081ba5c`), disassemble
(`arm-none-eabi-objdump --start-address=0x1d9c0 --stop-address=0x1db00`).
Found a plain reset-controller idiom, repeated at least twice in the same
function: write a masked value to `0xF0000000+0x54` (clearing one
specific bit), then spin reading that *same* offset until it reads back
`0xFFFFFFFF` (all bits set = "reset acked"). `0xF0000000` had never been
mapped at all before this - genuinely different from every earlier hang,
which were all modeled-but-wrong stubs; this address was silently eaten
by `ignore_memory_transaction_failures` (reads as 0, which never
satisfies `== -1`), with zero trace output pointing at it even with `-d
unimp` on, since nothing had ever logged an access to a region that
plain doesn't exist. Fixed with a new `im7cam.reset-ctrl` device:
offset `0x54` always reads back `0xFFFFFFFF`; everything else in the
block still falls through to the standard logging stub.

**Result**: with that fixed, the boot runs dramatically further - the
device's own real network/TFTP recovery flow now executes end to end:
`Using FH EMAC device`, a real TFTP attempt (`Download Filename
'upgrade_info_...txt'`), retries with a backup server, `failed.txt` as a
last resort, and finally `resetting ...` - a genuine watchdog/soft
reset the firmware itself triggers after exhausting recovery attempts,
not an emulator crash. This is this device's real, by-design behavior
when it can't find valid flash - matches the TFTP recovery mechanism the
sibling device-level investigation already documented from the
extracted `1_hwid.bin` U-Boot environment (the `da`/`dk`/`dr` command
shortcuts, `serverip`/`ipaddr` defaults) almost exactly. No PHY is
modeled (`***ERROR: auto negotiation timeout`) and no real TFTP server
is reachable from inside the container by default - both expected, not
new bugs.

**Real bidirectional UART, disassembly-confirmed, not guessed**: the
user's actual goal was interactive console access, which needs RX
(TX-only until now). Found the driver's own `getc()`-equivalent
(`arm-none-eabi-objdump --start-address=0xa8a0 --stop-address=0xaa18`):
poll offset `0x14` until bit 0 is set (RX data ready), then read the
byte from offset `0x0` - the *same* register the TX path writes to, one
shared data port for both directions, a completely ordinary UART
design. Implemented for real: `im7cam.uart` now has a one-byte RX
buffer fed by the real `-serial stdio` chardev via
`qemu_chr_fe_set_handlers()` (mirrors `qemu-ipq5018/board/mr80x.c`'s own
UART RX wiring), with proper backpressure (`can_receive` returns 0 while
a byte is still waiting to be read by the guest, so keystrokes don't get
silently dropped).

**But it doesn't help yet, for a reason that isn't a UART bug**: fed
newline keystrokes continuously through the whole boot (`docker run -i`
piping `printf '\n'` in a loop) and grepped the trace for any read of
UART offset `0x14` (RX status) across the *entire* boot - zero hits.
This device's flash-probe-failure recovery path (the one section 6/8
above actually reaches, since the RDID mismatch below is still open)
genuinely never polls the keyboard at all - it's an unconditional,
by-design automatic network-recovery loop, not an interruptible
autoboot countdown. Getting to a state that actually checks for a
keypress (`bootdelay`-gated autoboot on the *normal* `kload;bootm` path,
per the device's real env - see the device-level investigation) needs
the flash probe to succeed first, which is where the remaining RDID
mismatch below directly blocks the user's stated priority, not just a
"nice to have."

**RDID mismatch: substantially re-investigated, still unresolved.**
Section 6 left this as "probably an extra pointer indirection." Directly
disproven this session: found the single call site of the RDID-driving
function (`grep -n "bl\s*0x9d78"` across the whole disassembled binary -
exactly one hit), set a breakpoint at its entry, and confirmed live:
`r0` points at a RAM struct whose first word genuinely *is*
`0xF0E00000` (`x/1xw $r0` → `0xf0e00000`) - no extra indirection, `r4`
resolves exactly as this file already modeled. Went further: a hardware
watchpoint on `*(int*)0xf0e00060` (the FIFO address) confirmed writes
really do reach it, and enabling opcode-received logging confirmed the
real driver genuinely sends `0x9F` to this device (`"im7cam: spi RDID
opcode received"` appears in the trace, once per probe attempt, matching
`SF: Unsupported manufacturer` appearing twice). Formed and tested one
concrete hypothesis - the offset-8 "reset" trigger was clearing the
pending RDID response before U-Boot's own read-back loop ran - fixed it
(resp_buf's lifetime no longer tied to that toggle) and reran: **byte for
byte identical garbage**, unchanged. So that wasn't it either. Left
exactly here, not guessed further: the write path is now confirmed
correct at every point checked (call site, base address, opcode
delivery), yet the printed manufacturer bytes never change no matter
what's tried on the response side - meaning whatever actually produces
those specific bytes (`b0 e4 83 a0 00` / `02 00 00 00 9f`, consistently,
across every attempt) isn't coming from `im7cam_spi_next_byte()` at all.
Next real step, not attempted yet: single-step *forward* from the
confirmed opcode-write point (rather than working backward from the
print statement, which is what every attempt so far has done) to find
exactly which instruction produces each of the 5 printed bytes - almost
certainly reveals either a completely different read path (a "quick ID"
auto-sequence hardware feature this file hasn't found, matching the
"len is 5, not 3" oddity noted back in section 6) or a bug in how this
function's caller assembles/prints the 5-byte buffer that has nothing to
do with the SPI device model at all.

## Status (updated again)

Boots to the device's own real, complete recovery-mode flow -
banner → DRAM → env → **network/TFTP recovery attempt → reset** - not a
stub artifact anywhere in that path. UART is genuinely bidirectional now
(TX confirmed working since section 5; RX now implemented and
protocol-confirmed via disassembly, just not yet reachable because nothing
on the current boot path asks for input). The RDID mismatch is the one
remaining piece standing between here and an interactive console: it's
what's keeping the flash probe failing, which is what routes every boot
into the unconditional recovery flow instead of the normal, keypress-
interruptible `bootdelay` path. Next session should pick up exactly
where section 8 left off - forward-tracing from the confirmed opcode
write, not another attempt at guessing the response side.

## 9. RDID: two more disproven hypotheses, and a real methodology dead-end

Directly asked to resolve section 8's open RDID mismatch. Two more
concrete, testable fixes attempted this session; both disproven by
result, not by reasoning - genuinely tested against the real boot each
time, not assumed.

**Attempt A**: forward-traced from the confirmed opcode-write
instruction (`0xa0807db4`, single-stepped ~35 instructions with a
register dump per step) instead of working backward from the print
statement, per section 8's own stated next step. Found something
unexpected: the very next thing the driver does after writing the
opcode is call into a generic elapsed-time/delay helper - reads
`IM7CAM_TIMER_BASE+4` (this file's own timer, section 6) twice, computes
a delta via a NOT-based subtraction trick, and the very first delta
computed came out enormous (`0xFF5F7E1A`) because the "previous
timestamp" field it compared against was `0xFFFFFFFF` - an
uninitialized-looking sentinel, not a real prior reading. Stepped
further (60 more instructions) into what's recognizably a textbook
software `fls()`/count-leading-zeros bit-scan (successive `>>16/>>8/>>4`
threshold checks + a lookup table for the last nibble) - standard
generic C library code for turning a large tick count into a bit
position, almost certainly part of a `udelay()`-style tick-to-loop-count
conversion. Not itself proven broken - genuinely can't tell from this
alone whether a huge input here is normal (e.g. "wait until timeout"
math that's supposed to see a large distant target) or the actual bug.
Did not chase this further - it's deep inside U-Boot's own generic
timing library, not SPI-specific, and single-stepping through generic
library internals with no symbols has a very poor time-spent-to-insight
ratio compared to every other finding in this file, all of which came
from short, targeted disassembly windows around a specific known
address.

**Attempt B (tested, disproven)**: reasoned that the outer retry loop
(`0xa304`'s `bl 0x9d78 ... bne 0xa2e8`) likely calls the byte-transmit
path multiple times per probe, each bracketed by the offset-8 toggle
(which resets `cmd_len`), and that a *later*, unrelated first-byte in
one of those calls was hitting `im7cam_spi_write_byte()`'s `default:`
case, which (from section 8's own earlier fix) explicitly cleared
`resp_buf` - same root problem as section 8's offset-8 fix, just
reached through a different call. Removed the clear from the `default:`
case too. Rebuilt, reran: **byte-for-byte identical output**,
`b0 e4 83 a0 00` / `02 00 00 00 9f`, unchanged. Ruled out.

**The real finding, and why this needs a different method going
forward**: with both response-side fixes disproven, watched the
*destination* buffer directly instead of the FIFO - a hardware
watchpoint on `0xA037FF10` (the exact address confirmed live as the
`r1` destination-buffer argument at `0x9d78`'s own entry, via `x/1xw
$r0` at the breakpoint). Its value **before any of this session's
traced code even ran** was already `0xB0` - the literal first byte of
the printed "manufacturer" garbage. The two writes the watchpoint went
on to catch came from two unrelated-looking addresses
(`0xa080ed74`, `0xa080fd40`) with register state that doesn't
resemble anything SPI-related (r0/r1/r2/r3 values matching neither the
`IM7CAM_SPI_BASE` pointer nor any address this file has ever mapped).
Most likely reading: `0xA037FF10` is ordinary stack space genuinely
reused by unrelated functions between calls (completely normal - it's
just a stack address), and the *actual* RDID readback either targets a
different buffer than the one this session assumed, or the real
call chain the "SF: Unsupported manufacturer" message reports from
isn't the `0x9d78` function this file has been tracing at all - meaning
the working assumption connecting the confirmed-correct opcode write
(section 8) to the printed failure (section 6) may itself be wrong,
not just some detail of the response mechanism.

**Where this leaves things**: every concrete, testable hypothesis tried
so far (three total across sections 8-9) has been disproven by actually
rebuilding and rerunning against the real boot, not just reasoned about
- a real, honest track record, not a stall. But blind single-stepping
has now demonstrably hit diminishing returns for this specific problem:
each attempt costs a full gdbstub session plus a rebuild, and the last
two produced zero new information about *where the bug actually is*,
only where it isn't. Next session should change method, not just try
another guess:
1. **Confirm which call actually produces the printed message** first,
   before touching the device model again - find `"SF: Unsupported
   manufacturer"`'s real call site (the string's own runtime address
   didn't resolve via the direct literal-pool search used successfully
   for every other string in this file, per section 6 - that itself is
   a clue worth understanding, not a dead end to route around again).
2. Consider whether a **real vendor GPL source drop exists** for this
   specific driver (a generic-looking `spi_flash` probe layer, not
   obviously Fullhan-specific in its structure) rather than continuing
   pure black-box disassembly - this is the one place in this whole
   file where the "no GPL source, disassemble everything" constraint
   (unlike `qemu-ipq5018/board/mr80x.c`'s real vendor source) has
   genuinely slowed things down enough to be worth spending time
   searching for an exception, even though section 7 already looked and
   didn't find one for the SoC as a whole.

## 10. RDID: root cause found and fixed - real vendor source was the key

Directly asked to keep going, in a loop, until the kernel boots, always
documenting. Followed section 9's own second suggestion: searched for
real source for the generic-looking `spi_flash` probe layer instead of
continuing pure black-box tracing.

**It exists, publicly, and matches exactly.** `"SF: Unsupported
manufacturer"` plus a `u8 idcode[5]` buffer is real, stock U-Boot
2010.06 (`drivers/mtd/spi/spi_flash.c` at the actual `v2010.06` tag,
fetched from `github.com/u-boot/u-boot` - not a guess, this device's own
banner date-matches that exact release). Confirmed the real, generic
protocol: `spi_flash_cmd()` sends the opcode via one `spi_xfer()` call,
then reads the response via a second, separate `spi_xfer()` call into
`idcode[5]` - `SPI_FLASH_MAX_ID_LEN` really is 5 in this vintage, not 3,
exactly matching `"len is 5"`.

**Re-read this device's own `0x9d78` with that as a map, not a guess,
and found the actual bug**: `[r4,#0x24]` (a register this file had
*never* modeled - the old catch-all stub silently returned 0 for it) is
read once per read "chunk" and used *directly* as the byte count to
copy that pass (`r3 = r5 + *(r4+0x24)`, then the copy loop runs while
`r5 != r3`). With the stub returning 0, every chunk copied **zero
bytes**, unconditionally - the actual reason two separate, real fixes to
the response-side state machine in section 9 provably changed nothing:
the copy loop that would have consulted `resp_buf` never ran at all.
Fixed with `IM7CAM_SPI_REG_AVAIL` (offset `0x24`) always reporting `1` -
the simplest value that makes the copy loop advance exactly one real
byte per poll and terminate correctly regardless of how many bytes were
actually requested, with no risk of an overread the way returning a
guessed larger count could carry.

**First rebuild+test after that fix**: the manufacturer-mismatch garbage
was gone (no more `"SF: Unsupported manufacturer"` at all - progress!),
but `"Fail probe spi flash."` still printed, and a live `x/5xb` on the
confirmed destination buffer showed `1c ff ff ff ff` - the first byte
(the real Eon manufacturer ID) now correct, but bytes 2-3 (should be
`70 17`, this chip's real memory-type/capacity per its own datasheet -
section 6/7) still wrong. Traced with a temporary debug print inside
`im7cam_spi_next_byte()` itself (not more guessing - direct visibility
into the response buffer's own position) and found the real second bug
immediately: the read handler pulled `size` bytes (4, for the guest's
32-bit `ldr`) per single guest access, but the *real* byte-mode read
loop (`0x9e80`: `ldr r0,[fp]; strb r0,[r5],#1`) does a full 32-bit load
per real byte and keeps only the low 8 bits, issuing a fresh `ldr` for
the *next* real byte - not one 32-bit access draining 4 new FIFO bytes
at once (that's the *other* read path, `0x9e14`, genuinely 32-bit-wide,
used only when `sl==32`, not the `sl==8` byte-mode case this probe
actually takes). The old code silently burned 3 real response bytes per
guest read - exactly enough to explain `1c ff ff ff ff` (byte 0 correct,
then the response was already exhausted). Fixed: `IM7CAM_SPI_REG_DATA`
reads now pull exactly one byte from `im7cam_spi_next_byte()`
regardless of access size. Applied the identical fix to the write side
too (`im7cam_spi_write_byte()` was symmetrically over-consuming `size`
bytes per real byte written) - not yet observed breaking anything, but
the same bug class, and a real multi-byte `READ`/`FAST_READ` command
(opcode + 3 address bytes, each its own separate `str` in the confirmed
copy-loop idiom) would eventually hit it the exact same way RDID did.

**Result - the real breakthrough**: `"SF: Unsupported manufacturer"`
gone entirely (the JEDEC ID now reads correctly - `EN25QH64A` genuinely
exists as a named entry in a real parts table found in this binary's own
`.rodata`, right next to `XM25QH64C`/`EN25QH128A`, so this vendor fork
does recognize Eon parts despite the generic mainline source's stock
6-manufacturer switch not including it). U-Boot now issues **real
FAST_READ commands with real addresses** -
`opcode=0x0b addr=0x050000` (`0x0B` = FAST_READ, address `0x050000` -
exactly the `hwid`/env partition boundary per the confirmed partition
table, device-level investigation) - genuine forward progress past every
wall documented in sections 6-9 combined.

**New wall, further down than ever reached before**: the *very next*
run after this fix hangs at a **new** address, `0xE0300000` -
`bics r1,r0,r2; bne back`, an ordinary bit-test spin, but on a
peripheral base this file has never mapped at all (a different prefix
entirely from every `0xF0xxxxxx` block modeled so far). Not yet
investigated beyond confirming its existence - next step, same method
as every fix in this file: disassemble around the frozen PC's file
offset once its exact meaning is understood, don't guess. Recorded here
so this session's log doesn't lose the thread if it gets interrupted
before finishing that investigation.

## 11. Past the reset wall too - now stuck on partition-table parsing

Directly asked to keep looping - self-paced, autonomous continuation of
section 10.

Disassembled around the `0xE0300000` hang (file offset `0x25bb8`):
`bics r1,r0,r2; bne back`, polling offset `0x2c0` for a `1<<N` bit,
writing back to offset `0x338` once satisfied - reads like a generic
interrupt/sync-primitive wait (not confirmed which). Mapped as this
file's usual first move for anything new - a plain logging stub - then,
since a bare stub obviously wouldn't unblock a "wait for bit set" poll,
added the same kind of cheap "make it succeed" probe section 5/6 used
for the very first timer hang: offset `0x2c0` returns `0xFFFFFFFF`
unconditionally. Explicitly marked in the code as a probe, not a
citation-backed register model, unlike everything else in this file.

**Result**: past it immediately, and the "SF: Unsupported manufacturer"
war is conclusively over - not just quieter, actually gone, along with
"Can not find any available flash." (present in every single run before
section 10's fix, absent now). But a new, different failure appears:
`fail to load partition.txt from 60000` / `fail to init partinfo` -
loading the actual **partition table** (from the `partition` CramFS
partition at file offset `0x60000`, confirmed real per the device-level
investigation) still fails, even though basic flash communication now
demonstrably works (the ID probe proves that). Ends the same way as
every other unresolved-flash run: full TFTP recovery attempt, then
`resetting ...`.

**Working theory, not yet confirmed**: section 10 only fixed the
byte-mode (`sl==8`) read path, used by the small 5-byte ID probe. Loading
a whole partition-table file is a much larger read, plausibly taking the
*other* read path this file already found but never exercised - file
offset `0x9e14`'s genuinely 32-bit-wide FIFO drain (`sl==32`,
`ldr ip,[fp]; str ip,[r9,r0,lsl#2]`, four bytes actually consumed per
access, correctly, unlike the byte-mode bug). If that path has its own
version of section 10's bug #1 (relying on `IM7CAM_SPI_REG_AVAIL` to
report a real chunk size, currently hardcoded to always `1` regardless
of access width) it would make correspondingly little forward progress
per poll for a bulk 32-bit-word read, or interact with the "always 1"
answer in some other wrong way this session hasn't traced yet. Not
confirmed - the next concrete step, same method as every fix in this
file: get a real trace of *this specific* failure (which register
gets polled, what data actually comes back) rather than reasoning from
the code shape alone.

## Status (updated again, section 11)

Two real, load-bearing subsystems now confirmed correct end-to-end and
not just individually: UART (TX+RX, disassembly-confirmed protocol) and
the SPI flash ID probe (root cause found via real U-Boot 2010.06 source,
both the missing byte-count register and the size-blind FIFO
consumption bug fixed). The boot now gets further than at any earlier
point in this file's history - past the manufacturer-detection wall that
consumed most of sections 6-10 combined - before hitting a new, distinct
wall in partition-table loading. Likely the same *class* of bug
(chunk-size/available-count handling) hitting a different, larger-read
code path, not yet confirmed. Next step: real trace of the
`0x9e14`/32-bit-word path specifically.

## 12. `addr_latched` lifecycle chased through three bugs, a real ROM-mapping bug found and fixed, the "32-bit FIFO path" theory disproven, FAST_READ's *real* data path still not found

Autonomous continuation of section 11's open item, same `/loop` task
(bring the kernel up, document everything). Three separate sub-threads
this session, in the order they actually happened:

### 12a. `addr_latched` persistence - three fixes, the third one right

Section 11 left `addr_latched` never reset (stuck true forever once a
FAST_READ latched an address), which section 10's bug already showed
blocks the *next* transaction's address bytes from accumulating at all.
Chased through three attempts:

1. Reset `addr_latched` at the same offset-8 "routine bracket" toggle
   that section 9/10 already knew was NOT a real transaction boundary
   for `resp_buf` - wrong for the identical reason: this toggle fires
   constantly mid-transaction, so it cleared `addr_latched` before the
   real read-back loop ever ran. A live per-byte debug log in
   `im7cam_spi_next_byte()`'s `addr_latched` branch confirmed it: zero
   fires during an entire attempted 4096-byte partition read, even
   though `"spi read command...addr=0x060000"` had genuinely logged
   moments earlier.
2. Moved the reset to the *start* of a genuinely new opcode byte
   (`cmd_len == 0` in `im7cam_spi_write_byte()`) instead - correct
   *if* every byte reaching that branch really is a new opcode, which
   turned out to be false (see 12b): the controller's own dummy/filler
   clock bytes reach this exact branch too (their value doesn't matter
   to real hardware, only that a clock pulses), and treating every one
   of them as "transaction start" wiped `addr_latched` after exactly
   one real byte of the FAST_READ response - regression confirmed live
   (`read_addr=0x50000 byte=0xd2` logged once, then never again for the
   second command).
3. This is as far as the byte-FIFO path was pushed this session before
   12b's finding made the whole question moot for FAST_READ specifically
   - see below. The fix from attempt 2 is left in place (a real
   improvement over attempt 1 either way) but doesn't matter for the
   actual bulk-read wall, because...

### 12b. FAST_READ never touches the byte-FIFO at all - the "32-bit path" theory from section 11 is wrong

Section 11 guessed the stuck partition-table read might be hitting the
*other*, 32-bit-wide FIFO drain loop (file offset `0x9e14`) instead of
the byte-mode one section 10 fixed. Live PC-tagged tracing (see 12c for
why that took two attempts) disproves this directly: after a FAST_READ
command latches a real address (`addr=0x050000` or `addr=0x060000`,
both confirmed correctly decoded), **zero** subsequent accesses to
`IM7CAM_SPI_REG_AVAIL` (0x24) or `IM7CAM_SPI_REG_DATA` (0x60) ever
happen - not the byte-mode path, not a 32-bit-wide variant of it,
nothing. Confirmed two ways: a full instruction-by-instruction PC trace
from the command latch onward never revisits either offset, and a
*conditional* hardware breakpoint (`break *0xa0807e80 if $r11 !=
0xf0e00060`) on the one real byte-copy loop this binary is confirmed to
use (the one RDID's response drains through, `r11`/`fp` always pinned
to `0xf0e00060`, the DATA register) never fires across a full boot-to-
reset cycle - i.e. that shared copy routine is *only* ever invoked with
the FIFO as its source, RDID/RDSR bytes only, never for a FAST_READ's
bulk data.

What the trace shows instead, immediately after the address latches:
a short, fixed sequence (`off=0x54=7`, `off=0x4=<some length-shaped
value>`, an interrupt-enable-style bit toggle at `off=0x4c`, then a
single write of `0xFFFFFFFF` to a completely different peripheral block
at `0xE0300000+0x338`) with **no observed poll loop of any kind
afterward** - not of `0xE0300000`, not of the SPI block's own status
registers. The sequence is bit-for-bit identical (same fixed
`0xa08a0540` destination-looking value, same `0x18000000` and `0x208`
constants) regardless of which flash address or how large the requested
read is, which rules out it being a real per-transfer DMA descriptor
setup - a real one would encode the actual target address/length
somewhere in that sequence, and this doesn't. Genuinely inconclusive
- either this is an unrelated generic cache/sync primitive that
happens to run at a fixed point in the driver's flow (most likely,
given the "always identical values" evidence), or it's a DMA kickoff
whose real descriptor lives in a RAM structure this session didn't
locate. **Not fixed.** This is the actual open item now, not the
byte-FIFO.

One dead end explicitly ruled out to save future time: it is **not**
a memory-mapped ("XIP") flash-read window either, despite `im7cam.spi`
offsets `0x100`/`0x104` being programmed with the fixed constant
`0xB0000000` on *every* single transaction (RDID included) - looked
exactly like a hard-wired XIP base register at first. A real backing
RAM region was built at `0xB0000000` for this theory (see 12c for why
the first attempt at that was itself buggy) and independently confirmed
byte-correct via the HMP monitor (`xp /16xb 0xb0060000` → `45 3d cd 28
...`, the real CramFS magic, byte-for-byte matching the flash dump).
But a hardware watchpoint on a read of that exact address never fires
across a full boot-to-reset cycle - the guest genuinely never reads
from there. The region is left in place (harmless, and correctly built
this time - see 12c), but it is not the mechanism this driver uses.

Also traced, and also a dead end: the loader function's real arguments
(found by breaking on its entry, `0xa0821814`, rather than reusing a
now-stale destination-buffer address from an earlier session -
`r0`=dest buffer, `r2`=flash offset, both confirmed matching across two
calls for `partition.txt`/`partitionV2.txt`) point at buffer
`0xa0398160`. A write-watchpoint on it shows it oscillating between a
self-referential pointer value and `0`, then finally landing on
`0x80000003` (bit 31 set - reads as an explicit error/status code, not
data) - this buffer is a small request/status descriptor, not the
actual data destination. The real per-byte destination buffer is
further indirected through it and wasn't reached this session.

### 12c. A real, confirmed, independent bug: `rom_add_blob_fixed()` doesn't create a memory region

Found while chasing 12b's XIP-window theory, and worth its own
sub-heading because it's a genuine fix, unlike 12b's still-open
question. `rom_add_blob_fixed()` (the same call this file already uses
to load the U-Boot image itself at `IM7CAM_RAM_BASE`) only *queues* the
blob; the actual bytes get written into whatever `MemoryRegion` already
backs that address at machine-reset time
(`hw/core/loader.c`'s `rom_reset()` → `address_space_write_rom()`) - it
does **not** create a region itself. That's harmless when the target
address is already RAM (true for the U-Boot image), but a first attempt
at the section 12b XIP window called it against `0xB0000000` with
nothing mapped there at all, so it silently wrote nowhere - the HMP
monitor's `xp` confirmed "Cannot access memory" at that address even
after the "fix" was in place and had rebuilt cleanly. Fixed by building
a real backing region first (`memory_region_init_ram()` + a direct
`memcpy()` of the flash bytes + `memory_region_set_readonly()`) and
*then* adding it as a subregion, the same pattern `im7cam_add_uart()`
etc. already use for every other device in this file. Confirmed correct
per 12b above. Kept in the tree even though 12b shows it isn't (yet)
what unblocks boot, since it's real and cheap and may matter once the
real bulk-read mechanism is found (a legitimate memory-mapped fast path
is a very plausible *part* of the real answer, just not triggered the
way this session assumed).

### 12d. A gdbstub attach gotcha worth recording so it doesn't cost an hour again

Several watchpoint/breakpoint attempts this session hung until the
external `timeout` killed them, all producing the exact same misleading
symptom: gdb prints `Cannot execute this command while the target is
running` for every command after `continue`, even `printf`. Spent real
effort suspecting a broken/slow software-watchpoint single-step penalty
before finding the actual cause: `qemu-system-arm` was started **without
`-S`** (free-running) and gdb attached to it already-running via `target
remote`. Attaching gdb to an already-running QEMU target this way
doesn't reliably leave it in a state where `continue` behaves
synchronously in `-batch` scripts - every subsequent scripted command
races ahead of the still-running target and fails with that message.
Always start the target with `-S` (paused at the reset vector) for any
scripted gdbstub session in this file's workflow, even when the
breakpoint of interest is deep into boot - a plain software breakpoint
reached fast (this session's were all reached within a couple of real
seconds) costs nothing by starting from the very first instruction
instead of attaching mid-flight.

### 12e. Continuation checkpoint reproduced before new work

Before starting the next investigation, rebuilt the Docker image from
the exact working tree recorded in this section and booted it for 20
seconds with the original `0_U-Boot.bin` plus the real 8 MiB SPI dump.
The build completed cleanly and the runtime result reproduced the known
wall exactly: real U-Boot banner, 64 MiB DRAM, then `fail to load
partition.txt from 60000` / `fail to init partinfo`, followed by the TFTP
fallback. This is the baseline against which the next instrumentation
will be compared.

Also corrected two source comments and `run.sh`'s header while making
this checkpoint: the source had accidentally promoted the already-
disproved XIP hypothesis to fact, and the runner still described the old
section-6 JEDEC-ID failure. The XIP mapping itself remains in place for
the reasons in 12b/12c; only the claim about what the guest actually does
was corrected.

## Status (updated again, section 12)

UART and the SPI ID probe remain solid. The partition-table wall is
now understood precisely enough to rule out two plausible theories
(32-bit FIFO path, memory-mapped XIP window) with real evidence rather
than guesswork, and a real independent bug (`rom_add_blob_fixed()`
needing a pre-existing backing region) was found and fixed along the
way - confirmed harmless/correct, kept in the tree. The actual
mechanism FAST_READ uses to move bulk data - almost certainly *some*
kind of DMA/descriptor hand-off through `0xE0300000`, given the driver
demonstrably does something there right after every FAST_READ command
and before giving up - is still not identified. Next step: find where
the real per-transfer parameters (destination address, real length) get
written, if they exist at all outside of `0xE0300000`'s fixed-looking
sequence - possibly in a RAM-resident descriptor structure built
*before* the register pokes rather than in the registers themselves,
which this session didn't check.

## 13. FAST_READ DMA reconstructed; partition loading fixed; Linux reaches its ARM11 entry

Continued directly from section 12's open question. This round replaces
the last speculative `0xE0300000` workaround with a descriptor-driven
model based on the actual U-Boot execution path, then follows the newly
loaded kernel far enough to overturn the remaining CPU placeholder.

### 13a. The loader's real call chain and buffers

The earlier interpretation of the loader entry at runtime address
`0xA0821814` was partly wrong: `r0=0xA0398160` is a request/status object,
not the destination data buffer. Its other arguments are the filename
in `r1` and flash offset `0x60000` in `r2`; it tries
`partitionV3.txt`, `partitionV2.txt`, then `partition.txt`. The actual
4 KiB destinations observed dynamically are `0xA03B8208`,
`0xA03B82A8`, and `0xA03B8348`.

The complete read path, using runtime addresses (the 8 KiB image header
means runtime address = file offset - `0x2000` + `0xA0800000`), is:

1. generic read dispatcher `0xA0804314`, handle `0xA083E364`;
2. handle callback `0xA0803CC8`, context `0xA083E414`;
3. flash read implementation `0xA08065D8`, which builds opcode `0x0B`
   (FAST_READ) and calls `0xA0804C60`;
4. `spi_xfer` at `0xA0808398`, whose transfer implementation
   `0xA08080FC` selects PIO at `0xA0807D78` for at most 255 units and
   the large-transfer path `0xA0808028` otherwise;
5. `0xA0808028` builds an 84-byte stack configuration and calls the DMA
   helpers around `0xA0823B60`.

The pre-conversion configuration exposes destination at `+0x30`, count
at `+0x40`, width shift at `+0x10`, and the SPI DMA port
`0xF0E01000` at `+0x18`. For a partition-table transfer the observed
values were destination `0xA03B8208`, count `0x800`, width shift 1:
exactly 4096 bytes.

### 13b. `0xE0300000` is the DMA controller

Helper `0xA0823BCC` converts that configuration into linked, five-word
hardware descriptors in guest RAM:

| Offset | Meaning |
|---:|---|
| `+0x00` | source address |
| `+0x04` | destination address |
| `+0x08` | next descriptor |
| `+0x0C` | control, including transfer-width shift |
| `+0x10` | transfer count |

The previously mysterious block is consequently confirmed as DMA. Its
channel stride is `0x58`; channel-relative offset `0x10` receives the
descriptor address; global offset `0x3A0` starts enabled channels;
`0x2C0` reports completion bits; and writing those bits to `0x338`
acknowledges/clears them. The old catch-all behavior that returned
`0xFFFFFFFF` at `0x2C0` has been removed rather than retained beside the
real model.

`im7cam.c` now implements only the proven peripheral-to-memory case. It
reads little-endian descriptors from guest RAM, accepts the confirmed
SPI port as source, copies bytes from the currently latched flash
address to each validated RAM destination, advances along `next`, sets
the channel completion bit, and implements the acknowledge. Bounds,
width/count, source, and a 4096-descriptor chain cap are checked so a bad
guest descriptor cannot turn into an arbitrary host access.

The first version executed only the head descriptor. That was sufficient
for every 4 KiB partition-table attempt and removed `fail to load
partition.txt from 60000`, proving the original wall was fixed, but the
kernel load exposed the missing link traversal: U-Boot represents each
64 KiB read as sixteen linked 4 KiB descriptors. Following the chain
fixed that second issue. Trace verification now reports, among others:

```text
SPI DMA ch=0 flash=0x050000 bytes=0x10000 descs=16
SPI DMA ch=0 flash=0x060000 bytes=0x1000 descs=1
SPI DMA ch=0 flash=0x070000 bytes=0x10000 descs=16
...
SPI DMA ch=0 flash=0x1a0000 bytes=0xb500 descs=12
```

That final range completes the uImage whose header at flash `0x70000`
names `Linux-4.9.129`, data size `0x13AC08`, load/entry address
`0xA0008000`, and no compression at the uImage layer.

### 13c. Small PIO reads need dummy clocks to preserve FAST_READ state

Once partition parsing worked, U-Boot performed a small PIO read at
flash `0x70000`. The controller brackets command and receive phases with
the same offset-8 toggles already discussed in section 12, resetting
`cmd_len`; the receive phase then writes `0xFF` solely to generate SPI
clocks. Treating that filler as a new opcode cleared `addr_latched` and
lost the read. `im7cam_spi_write_byte()` now recognizes `0xFF` in this
specific state as a dummy clock and preserves the address. The trace
then produced the correct first uImage byte (`read_addr=0x70000
byte=0x27`) and proceeded into all bulk kernel reads above.

### 13d. The kernel proves ARM11/ARMv6; `cortex-a7` was the wrong placeholder

With DMA fixed, U-Boot copies the image to `0xA0008000` and enters it.
Under the old `cortex-a7`, gdb finds PC in the deliberate zImage error
loop at `0xA0009478`. The path is unambiguous: entry code reads MIDR into
`r9` (`0x410FC075`), calls the lookup routine at `0xA0009430`, receives
zero in `r5`, then branches to that loop from `0xA000803C`.

The embedded `proc_info` range contains exactly one 52-byte entry at
`0xA032CCA0`: ID `0x0007B000`, mask `0x0007F000`. This is the ARM11 /
ARMv6 identification pattern, not Cortex-A7/ARMv7. Running the same
machine with QEMU's `arm1176` model satisfies it and reaches the linked
kernel proper (live PC observed at `0xC01F44CC`, rather than the zImage
error loop). The board default is therefore changed from `cortex-a7` to
`arm1176`; this is now evidence from the device's own kernel, while still
supporting the VBAR access that ruled out `arm926` in section 4.

### 13e. New wall

There is no readable Linux console output yet. With `arm1176`, a
15-second `-d unimp` run grows to millions of repeated reads from timer
block `0xF0C00000`, offset `0x18`, while the existing minimal timer only
advances offset `0x04`. A live PC in the linked kernel plus its virtual
mapping confirms Linux itself is now executing; the next task is to
disassemble the offset-`0x18` consumer and model the register semantics
(very likely another/current timer channel) from the poll condition.

## Status (updated again, section 13)

The original partition-table failure and the entire SPI bulk-read path
are resolved. U-Boot reads the real partition metadata, loads the full
Linux uImage through linked DMA descriptors, and enters its zImage. The
kernel's own `proc_info` table identifies ARM11/ARMv6 and the `arm1176`
QEMU model reaches linked kernel code. The current first kernel-side wall
is the unmapped behavior of timer offset `0x18`; this is the next focused
bring-up target.

## 14. Linux clocksource at timer `+0x18`

Section 13's repeated reads were localized with a hardware read
watchpoint on the kernel's virtual address `0xFE010018`. It fires at
`0xC01F43D4` in this compact callback:

```text
c01f43cc: ldr r3, [pc, #16]   ; address of timer pointer
c01f43d0: ldr r3, [r3, #4]   ; r3 = 0xfe010018
c01f43d4: ldr r0, [r3]       ; raw hardware count
c01f43d8: mov r1, #0
c01f43dc: mvn r0, r0          ; expose an increasing clocksource
c01f43e0: bx  lr
```

The old unimplemented read returned zero, so the callback returned
`0xFFFFFFFF` forever. Kernel delay/time calibration consequently made
millions of calls without observing elapsed time. Timer offset `0x18`
is now backed by the complement of QEMU virtual-clock ticks: it behaves
as a descending 32-bit hardware counter, and the driver's own `mvn`
turns it back into the increasing value it expects. Offset `0x04`
retains its existing increasing counter behavior for U-Boot.

This is intentionally a semantic minimum, not a claim about the real
clock frequency, reload registers, or interrupt routing. Those require
separate evidence; the presently confirmed requirement is wraparound
32-bit progress with the direction shown above.

The first version stopped there and exposed `+0x18` unconditionally.
It removed the log flood (about 4.45 million lines became about 13,300
in a comparable trace) and let Linux configure both timer channels, but
a subsequent gdb stop still landed in `0xC01F44C4`--`0xC01F4510`. That
second loop reads channel 0's current value at virtual `0xFE010004`
(physical `+0x04`) and waits for zero after disabling the channel. The
old section-6 approximation returned virtual-clock nanoseconds at
`+0x04` even while disabled, so Linux could only escape accidentally at
a 32-bit wrap.

The timer model is consequently stateful at the minimum level supported
by the trace. It records the two observed channel triplets:

| Channel | load | current value | control |
|---:|---:|---:|---:|
| 0 | `+0x00` | `+0x04` | `+0x08` |
| 1 | `+0x14` | `+0x18` | `+0x1C` |

Control bit 0 gates each counter. A disabled channel reads zero, which
matches the kernel's stop-and-wait sequence; enabled channel 0 exposes
the increasing virtual ticks U-Boot already relies on, while enabled
channel 1 exposes their complement for Linux's `mvn` clocksource. Load
values and full control words are retained even though reload/IRQ
semantics are not modeled yet. This preserves the evidence boundary:
only behavior exercised by the real boot path is implemented.

The rebuilt image confirms both parts of the fix. Timer `+0x18` no
longer appears as unimplemented, the repeated-read flood is gone, and
Linux gets beyond the timer-channel setup. A later gdb stop now lands at
`0xC0009D94` in the standard early delay-calibration shape:

```text
c0009d8c: ldr r2, [r8]       ; snapshot a global tick value
c0009d90: str r4, [r3]
c0009d94: ldr r3, [r8]
c0009d98: cmp r2, r3
c0009d9c: beq c0009d94       ; wait for the first tick to change
```

The value never changes because this skeleton has no interrupt
controller or timer IRQ wired to the CPU. This is a genuinely new wall,
not another counter-direction issue: the clocksource progresses, but no
clock-event interrupt advances the kernel tick. The next bring-up step
is to identify the interrupt-controller block and the IRQ selected by
the channel-0 timer setup, then assert that line according to the
observed timer control/reload state.

## Status (updated again, section 14)

Both timer channels now provide the minimal counter/control semantics
the real boot path demonstrates. Linux completes its timer setup and
reaches delay calibration. Boot is presently blocked waiting for its
first timer interrupt; interrupt-controller discovery and timer IRQ
wiring are the next focused target.

## 15. Hidden printk log recovered: FH8626V100, 1 MHz timer, IRQ 19

The serial console is not yet a reliable way to see Linux output, but
the messages already accumulated in RAM. Attached gdb to the running
kernel and dumped virtual range `0xC0000000`--`0xC0800000`; printable
records in the in-memory printk ring provide several new facts from the
guest itself:

```text
Hardware name: FH8626V100
Switching to timer-based delay loop, resolution 1000ns
clocksource: timer1: mask: 0xffffffff ...
sched_clock: 32 bits at 1000kHz, resolution 1000ns, ...
console [tty0] enabled
console [ttyS-1] enabled
```

This corrects another approximation: timer ticks are 1 MHz, not raw
nanoseconds. Both emulated current-value registers now derive ticks as
`QEMU_CLOCK_VIRTUAL / 1000`, preserving the already-proven directions
and enable gating while matching the frequency Linux registered.

The clock-event allocation is also directly inspectable. The object at
`0xC2013000` contains `name=0xC02D7D89` (`"timer0"`), rating 300, and
IRQ field `0x13` (19), using the Linux 4.9
`struct clock_event_device` layout. Its Fullhan-private tail contains
MMIO base `0xFE010000` and frequency `0x000F4240` (1,000,000), an
independent confirmation of both findings.

One diagnostic dead end is worth preserving. `-d guest_errors` exposed
large volumes of otherwise suppressed unmapped traffic at physical
`0xE2000000`, `0xE0600000`, and a few accesses at `0xE0200000`.
The million-read `0xE2000044` loop is not the Linux interrupt
controller: a reset-to-boot gdb read watchpoint catches it in U-Boot at
`0xA082587C`, testing status bit 2 with a one-million-iteration timeout.
The sibling FH8852 register map further identifies `0xE0600000` as
GMAC, so neither address should be promoted to an interrupt-controller
model from access shape alone.

The in-RAM log also contains repeated `Division by zero in kernel`
backtraces during early clock calculations. Linux survives them and
registers the 1 MHz fallback/derived timer correctly, but they remain a
separate clock-tree fidelity problem to revisit after interrupts allow
normal boot diagnostics.

## Status (updated again, section 15)

The SoC identity is now device-confirmed as FH8626V100, timer frequency
as 1 MHz, and the first required clock-event interrupt as IRQ 19. The
kernel remains in `calibrate_delay()` waiting for the global tick to
change. Next step: recover the Fullhan interrupt-controller dispatch and
mask/ack protocol from the live kernel, wire timer0 to IRQ 19, and then
use the newly functional tick to continue boot.

## 16. `run.sh` no longer requires a TTY

Testing the documented invocation verbatim exposed a runner bug before
QEMU itself started: the script unconditionally passed `-it` to
`docker run`. Docker rejects `-t` when stdin is a pipe or the command is
launched by automation, with `cannot attach stdin to a TTY-enabled
container because stdin is not a terminal`.

The runner now checks both stdin and stdout with `[[ -t ... ]]`. In a
real interactive terminal it retains `-it`, including QEMU's `Ctrl-A X`
escape; otherwise it uses `-i` without allocating a pseudo-TTY. The same
published command therefore works interactively and in non-interactive
test/automation environments.

## 17. Timer IRQ routing, visible Linux console, and SPI capability probe

The section-15 IRQ number was incomplete. Disassembling the live high-vector
IRQ path first located `handle_arch_irq` through the pointer at `0xC03397E8`.
The handler at `0xC0009360` scans 64 pending bits starting at virtual
`0xFE000030` with `find_first_bit()`, which uses **byte** loads, and then calls
`irq_find_mapping()` before generic dispatch. A `-d guest_errors` run proves
those virtual accesses land at physical `0xE0200030`--`0xE0200037`; the
earlier `0xE0000000` inference was wrong.

The first interrupt-controller prototype exposed two more useful failures:

1. accepting only word reads made the byte-scanning handler report repeated
   `unexpected IRQ trap at vector 00`; pending reads now support every slice
   of the eight-byte bitmap;
2. routing pending bit 19 made `irq_find_mapping(domain, 19)` return Linux IRQ
   35, which had no timer action. The clock-event object at `0xC2013000`
   actually stores Linux IRQ 19, not hwirq 19. This domain has a +16 virtual
   IRQ offset, so Linux IRQ 19 is hwirq 3. Kernel initialization independently
   confirms that conclusion by writing `0x8` to controller offset `+0x00`.

The IRQ-chip callback reads controller `+0x08`, ORs `BIT(hwirq)`, and writes
it back. During the deliberately wrong hwirq-19 experiment this was the
repeated value `0x00080000`, identifying `+0x08` as the low pending
acknowledge. The model now exposes the physical controller at `0xE0200000`,
asserts the ARM CPU IRQ input for timer hwirq 3, returns its pending bit at
`+0x30`, and clears it on the `+0x08` acknowledge.

Timer tracing provided the exact periodic setup:

```text
timer0 control=0x2 load=0x5f5e100
timer0 load=0x2710 control=0x2
timer0 control=0x3 load=0x2710
```

`0x2710` is 10,000 ticks at the already-proven 1 MHz clock, i.e. 100 Hz.
Control bit 0 enables the counter and bit 1 is auto-reload. A QEMU virtual
timer now expires after `load * 1000` ns, sets timer status bit 0 at `+0xA0`,
raises hwirq 3, and automatically reloads while control bit 1 remains set.
This advances the kernel tick and gets Linux out of `calibrate_delay()`.

### Why Linux output was still invisible

The kernel had been progressing silently, exactly as the user's failed test
reported. Its in-memory console object begins at virtual `0xC034C038` and
contains the literal name `ttyS`; the signed index at `+0x2A` remains `-1`.
The registered console write callback at `0xC0191E30` starts with:

```text
ldrsh r12, [r0, #42]
cmp   r12, #0
bxlt  lr
```

That directly explains both `console [ttyS-1] enabled` in the hidden printk
ring and the complete absence of kernel UART accesses. A live gdb experiment
changed only `0xC034C062` from `0xFFFF` to zero; subsequent kernel messages
immediately appeared through the existing UART model. The machine now applies
that exact vendor-kernel compatibility fix in physical RAM
(`0xA034C062`) only when the neighboring object still has the `ttyS` signature
and the index is exactly `-1`. No firmware file is modified.

### Linux SPI probe and the next wall

With timer IRQs working, Linux originally reached PID 1 and then hit a kernel
`BUG()` at `0xC01AD83C`. This was not corrupt SquashFS or an incompatible
BusyBox: the authorized firmware-analysis directory confirms `/sbin/init` is
a symlink to an intact ARMv4T/EABI5 BusyBox. Disassembly shows the failing
function is the Fullhan SPI driver's capability assertion over status register
`+0x28`. It requires bit 9 and bit 2 set while bits 10, 3, and 0 are clear.
The old status value `0x4` was enough for U-Boot's `(status & 5) == 4` poll but
not for Linux; `0x204` is the minimal value satisfying both binaries.

After rebuilding, the exact public runner command was tested for 35 seconds
non-interactively. It now produces 429 lines and visibly reaches, among other
milestones:

```text
1902.18 BogoMIPS (lpj=9510912)
clocksource: Switched to clocksource timer1
fh_dmac fh_dmac.0: FH DMA Controller, 6 channels
Serial: fh serial driver
ttyS.0: ttyS0 at MMIO 0xf0700000 (irq = 34, ...)
0x000000000000-0x000000050000 : "U-Boot"
card0 connected!
NET: Registered protocol family 17
init_machine_late
```

The Linux console is therefore visibly functional and the prior
banner-only result is fixed. The current observed wall is later device
initialization: the unmodeled MMC controller repeatedly prints
`voltage switch read MCI_RESP0..3 : 0x0`. Separate repeated division-by-zero
diagnostics remain evidence of missing clock-tree register values; Linux
continues past them, so those and MMC are the next fidelity targets.

## Status (updated again, section 17)

The documented runner now visibly boots the real Linux 4.9.129 kernel well
beyond early initialization. SPI flash/DMA, the clocksource, periodic timer
interrupt, minimal interrupt controller, Fullhan SPI capability status, and
the vendor console-index compatibility fix are active. Full boot is not yet
claimed: MMC voltage-switch polling and incomplete clock-tree values are the
next blockers.

## 18. Two MMC hosts, direct MMU translation, and the real rootfs wall

The repeated `voltage switch read MCI_RESP0..3 : 0x0` was investigated with
the authorized kernel and flash artifacts under
`openwrt-build-tools/tools/firmware-lab/work/miboim7-tudo-sobre`; they were
read-only reference inputs and were not modified.

The live request routine at `0xC01F0800` reads its host MMIO base from object
offset `+0x10`. Its command-status loop reads MMIO `+0x44`; bit 2 takes the
completed-command path, while bit 10 reaches the driver's literal
`irq cmd status stat = 0x%x is timeout error!`. With neither bit present the
routine retries up to 100 times, reads response words, stores `-ETIMEDOUT`,
and produces the visible voltage-switch retry. This agrees with the common
DesignWare MMC raw-interrupt layout, but the conclusion here comes from this
kernel's own branches and strings rather than that resemblance.

The callback at `0xC01EF8D0` reads MMIO `+0x50`, masks bit 0 and returns it.
The first prototype incorrectly called that bit `ready` and retained all
unknown writes. The trace disproved both choices:

- a write of 1 to `+0x10` followed by 51 reads is a self-clearing reset wait;
  retaining the write made the bit remain stuck, so unknown writes are again
  trace-only;
- changing `+0x50` from zero to one changed the visible kernel result from
  `card0 connected!` to `card0 disconnected!`. Bit 0 therefore means card
  absent, not ready.

An earlier guest-error trace appeared to associate the host with
`0xE0700000`, but that was only temporal correlation with another block. The
decisive check used QEMU's page translation while the driver was stopped:

```text
host0 object 0xC21CAA40, MMIO VA 0xC2A04000
gva2gpa 0xC2A04000 -> 0xE2000000

host1 object 0xC21CAE40, MMIO VA 0xC2A0C000
gva2gpa 0xC2A0C000 -> 0xE2200000
```

As a cross-check, the provisional model at `0xE0700000` returned command-done
when read physically with monitor `xp`, while the stopped driver's `r7` still
contained zero. The provisional address was removed. Two narrow MMC regions
now exist at the directly translated physical bases. Because no SD/SDIO
image is attached, each exposes card-absent at `+0x50`; no fake card protocol
or data contents are claimed.

The exact normal runner command was rebuilt and tested for 60 seconds. The
result changed materially:

```text
card0 disconnected!
card1 disconnected!
NET: Registered protocol family 17
init_machine_late
hctosys: unable to open rtc device (rtc0)
List of all partitions:
1f04            5696 mtdblock4  (driver?)
No filesystem could mount root, tried:  squashfs
Kernel panic - not syncing: VFS: Unable to mount root fs on unknown-block(31,4)
```

The voltage-switch loop is gone. The new wall is the real root filesystem
read through Linux's SPI/MTD path: partition discovery succeeds and identifies
the correct `mtdblock4`, but SquashFS cannot read/mount its contents. This is
now the next fidelity target; the kernel panic is not presented as a complete
boot.

## Status (updated again, section 18)

The documented runner visibly boots Linux through all built-in device probes
and reaches the root mount attempt. Both directly identified MMC hosts are
coherently reported absent when no card image is supplied, eliminating the
previous endless voltage-switch diagnostics. Full boot is still not claimed:
the Linux SPI/MTD read path does not yet supply a mountable SquashFS image from
the real flash-backed `mtdblock4`, and missing clock-tree values still produce
non-fatal division-by-zero diagnostics during probe.

## 19. Linux SPI word width and the first real userspace boot

The `mtdblock4` failure was data corruption at the FIFO width boundary, not a
bad SquashFS image. A trace of Linux's mount attempt showed valid FAST_READ
commands at flash offsets `0x1D0000` and `0x1D0200`; the backing image returned
the expected first bytes (`0x68` and `0x5A`). Unlike U-Boot's large reads,
Linux did not start the already modeled external DMA engine for these blocks.
It drained the SPI FIFO through its PIO transfer loop.

Disassembly of the vendor SPI routine at `0xC01AD3D0`--`0xC01AD738` exposes
three distinct FIFO paths selected by the software bits-per-word value:

```text
8 bits:  ldr FIFO; strb to buffer
16 bits: ldr FIFO; strh to buffer
32 bits: ldr FIFO; str  to buffer
```

The previous QEMU model always consumed and returned one real flash byte per
32-bit MMIO read. That behavior remains necessary for U-Boot's proven 8-bit
path, but it corrupts a 16-bit transfer by making every stored halfword
`real_byte, 0x00`.

A hardware breakpoint at `0xC01AD470`, after the kernel computes the transfer
width but before it enters the receive loop, captured the first rootfs read:

```text
r1 = 0x200       receive mode
r4 + 0x60 = 0x200 bytes pending
r6 = 0xC2590000  destination buffer
r5 = 0x10        16 bits per word
```

The driver's own configuration routines provide an exact hardware encoding:
CTRL0's low nibble is `0xF` for 16-bit words, while advanced-control register
`+0xF4` bit 16 selects 32-bit words. Both registers are programmed with
read-modify-write sequences, so the model now preserves them. A FIFO read
packs one, two, or four sequential flash bytes in little-endian order based on
that live configuration. U-Boot's 8-bit behavior is retained; Linux's 16-bit
SquashFS reads now deliver intact halfwords.

After rebuilding, the exact non-trace runner command was tested for 90
seconds. It produced 1,224 visible serial lines and passed the former panic:

```text
card0 disconnected!
card1 disconnected!
VFS: Mounted root (squashfs filesystem) readonly on device 31:4.
devtmpfs: mounted
Freeing unused kernel memory: 104K
Video Memory Manager
[OSA-DRV] Char device create OK !
[DH_Binder] Binder Initial OK !
[prc] Success to init module!
[pdc] hwidName:IPC-S21F-imou, default:666 , please check!!!
[pdc] Wifi init: powerGpioCfg = 14
```

This is the first confirmed execution of the real vendor userspace, not just
the kernel. The run remains intentionally incomplete as a hardware replica:
proprietary media modules report unresolved dependencies, JFFS2 configuration
reads encounter erased-looking data, sensor/motor operations fail, and the
vendor watchdog counts down because those peripherals are not modeled. These
messages occur after PID 1 and substantial vendor initialization, so they are
new peripheral-fidelity targets rather than a bootloader/kernel/rootfs wall.

## Status (updated again, section 19)

The documented command now boots the extracted U-Boot, real Linux 4.9.129,
the flash-backed SquashFS root on `mtdblock4`, and substantial IPC-S21F vendor
userspace. This is a working analysis boot, but not a claim of complete camera
emulation: configuration JFFS2, clocks, media hardware, GPIO/sensor/motor and
watchdog behavior remain incomplete and visibly report errors.

## 20. Default full-SPI boot, matching the mr80x workflow

The user requested the same interface as `qemu-ipq5018`: provide one complete
flash dump, start at its bootloader partition, and let that real bootloader
load the kernel and rootfs from the same emulated flash. Until this section,
`im7cam` already executed the latter chain but still required the extracted
`0_U-Boot.bin` as a positional `-kernel` bootstrap. That was equivalent to
mr80x's explicit development override, not its default full-flash mode.

The im7 physical dump begins with the exact `0x50000`-byte U-Boot partition.
When no positional override is supplied, the machine now performs the final
handoff normally supplied by pre-U-Boot mask-ROM/SPL stages:

1. keep the complete, unmodified 8 MiB file attached to the SPI controller;
2. take partition 0 (`0x00000`--`0x4FFFF`) from that same backing buffer;
3. skip its proven `0x2000`-byte vendor header and load the remaining
   319,488 bytes at U-Boot's linked address `0xA0800000`;
4. enter U-Boot, which itself reads partition metadata, boot arguments,
   kernel and rootfs through the modeled SPI controller.

The proprietary pre-U-Boot stages are not claimed to be emulated, just as
mr80x models the final SBL/QSEE-to-APPSBL handoff. A positional
`0_U-Boot.bin` remains available as an explicit development override.

The runner no longer manufactures a default positional filename. It mounts
only the SPI dump and omits QEMU `-kernel` in normal mode. If neither a usable
SPI dump nor an explicit override exists, it stops with a clear error.

The requested full-SPI-only command was tested for 75 seconds:

```sh
./qemu-fullhan-im7/run.sh \
  --spi-image /media/dados_2tb/opw/openwrt-build-tools/tools/firmware-lab/work/miboim7-tudo-sobre/miboim7-spi-en25qh64-8mb-20260819.bin
```

It produced 1,228 serial lines and confirmed the complete chain:

```text
Boot source: U-Boot partition inside ...miboim7-spi...bin
im7cam: loaded 'SPI partition 0' (319488 of 327680 bytes, skipped 8192-byte header) at 0xa0800000
U-Boot 2010.06 (Sep 27 2024 - 18:56:13)
VFS: Mounted root (squashfs filesystem) readonly on device 31:4.
Freeing unused kernel memory: 104K
[pdc] hwidName:IPC-S21F-imou, default:666 , please check!!!
[pdc] Wifi init: powerGpioCfg = 14
```

Because this workspace's real dump path is also the runner's auto-detected
fallback, plain `./qemu-fullhan-im7/run.sh` selects the same full-SPI boot.

## Status (updated again, section 20)

Normal invocation now takes only the complete SPI dump and follows the real
observable chain from its U-Boot partition through kernel and SquashFS vendor
userspace. The separately extracted U-Boot is no longer required for normal
use. Pre-U-Boot ROM/SPL execution and the peripheral limitations listed in
section 19 remain outside the currently confirmed model.

## 21. The hidden OEM U-Boot console and reliable `--stop-autoboot`

The full-SPI path exposed a misleading console symptom: after the U-Boot
banner, only several blank CR/LF pairs appeared and Linux output began
immediately. The flash environment itself is valid and explicitly contains
`bootdelay=1`, `stdin=serial`, `stdout=serial`, and `stderr=serial`; it has no
`silent` variable. Nevertheless, continuously feeding Enter did not stop the
boot and the expected `Hit any key to stop autoboot` text never appeared.

Two independent problems were identified. First, the emulated one-byte UART
correctly stopped accepting host input while full, but failed to call
`qemu_chr_fe_accept_input()` after the guest consumed that byte. Consequently,
an early rejected key permanently prevented later input from reaching QEMU.
The RX data read now re-enables its chardev frontend as soon as the buffer is
empty.

Second, this is not an unmodified upstream U-Boot autoboot path. Disassembly
of the exact partition around `0xA0811A6C`--`0xA0811B4C` shows an OEM gate:

```text
0xA0811A6C  call tstc
0xA0811A70  compare result with zero
0xA0811B40  call getc
0xA0811B48  compare character with 42 ('*')
```

Any byte other than ASCII `*` is discarded by that first gate. The familiar
`Hit any key to stop autoboot: %2d` string does exist, and its standard
countdown routine is visible later at `0xA0811B50`, but the production path
does not expose it as the user's first interaction. This explains why Enter,
space, and the banner-based expectation all failed.

The conclusion was tested against the unmodified full SPI dump after fixing
RX re-arming. Feeding `*` followed by newline (the first byte opens the OEM
gate; the second aborts the following standard countdown) immediately
produced the real vendor console:

```text
U-Boot 2010.06 (Sep 27 2024 - 18:56:13)
disable wdt
>
```

Continued input was parsed by the genuine command interpreter (`Unknown
command '*' - try 'help'`), confirming this was not fabricated output.

`run.sh --stop-autoboot` now mirrors mr80x's timing-independent analysis
option while respecting this firmware's actual two-stage protocol: the board
pre-seeds `*` in UART RX before the CPU starts and, when U-Boot consumes it,
queues a second stop byte for the immediately following countdown. It logs
both guest reads. The SPI dump remains read-only and unchanged.
Normal invocation does not seed input and retains the complete automatic
U-Boot -> kernel -> SquashFS userspace chain.

Use:

```sh
./qemu-fullhan-im7/run.sh --stop-autoboot \
  --spi-image /media/dados_2tb/opw/openwrt-build-tools/tools/firmware-lab/work/miboim7-tudo-sobre/miboim7-spi-en25qh64-8mb-20260819.bin
```

At the `>` prompt, commands such as `help`, `printenv`, and the environment's
`kload 0xA1000000; bootm 0xA1000000` can be exercised interactively.

Final validation used that exact public runner command after a clean image
rebuild. Both staged-byte consumption messages appeared, `disable wdt` and
the `>` prompt followed, and neither Linux `BogoMIPS` nor `Starting kernel`
appeared during the test window. Piping `version` afterward reached the real
parser and returned this build's expected `Unknown command 'version'` plus a
fresh prompt, proving post-stop RX re-arming. A separate 25-second run without
`--stop-autoboot` still reached `VFS: Mounted root (squashfs filesystem)` and
vendor userspace (`[pdc] hwidName:IPC-S21F-imou`), so the explicit analysis
mode does not alter the default production boot.

## Status (updated again, section 21)

The production boot remains automatic by default. A separate, explicit
analysis mode now enters the real U-Boot command prompt deterministically,
and repeated terminal input works after each byte is consumed. The missing
visible countdown is documented as OEM firmware behavior, not emulated timer
speed or a missing environment variable.

## 22. Correction: board-like autoboot is the default, not an option

The user correctly rejected section 21's `--stop-autoboot` interface: a flag
that unconditionally enters the prompt is useful for debugging, but it is the
opposite of the physical board's observable behavior. The board displays a
short countdown, accepts a key during that window, and otherwise continues to
the kernel. The emulator must do that on its ordinary invocation.

The section 21 disassembly still explains the vendor binary: it contains a
private `'*'` gate immediately before its standard `bootdelay=1` loop. The
revised model satisfies only that internal gate. It does **not** inject the
second byte that aborts autoboot. After U-Boot consumes the private unlock,
UART RX is empty and re-armed, so the next byte can only be a real user key.
The guest's own countdown loop and `bootcmd` decision remain authoritative.

The production environment also redirects the countdown text away from the
emulated UART after the banner even though the loop executes. At the exact
point U-Boot consumes its internal unlock, the UART model therefore exposes
the same line the physical-board user expects:

```text
Hit any key to stop autoboot:  1
```

This is a console-visibility compatibility shim, not a replacement timer or
host-side decision: waiting and branching are still performed by the real
U-Boot code. If no host byte arrives, it runs the real `bootcmd`, loads Linux
from SPI and continues normally. If a host byte arrives during that one-second
guest window, U-Boot consumes it and enters its real `>` command interpreter.

`--stop-autoboot` and `IM7CAM_STOP_AUTOBOOT` were removed. The intended command
is again simply:

```sh
./qemu-fullhan-im7/run.sh \
  --spi-image /media/dados_2tb/opw/openwrt-build-tools/tools/firmware-lab/work/miboim7-tudo-sobre/miboim7-spi-en25qh64-8mb-20260819.bin
```

That exact public command was rebuilt and tested in both branches. With no
stdin byte it printed the countdown, then reached Linux `BogoMIPS`, mounted
the real SquashFS root and reached `[pdc] hwidName:IPC-S21F-imou`. Repeating
the same command while delivering one real `x` byte during the window printed
the countdown followed by `disable wdt` and the real `>` prompt; no Linux
milestone appeared during the test. No runner option or environment variable
distinguished the two runs--only whether the user supplied a byte.

## Status (updated again, section 22)

Normal invocation now has the physical-board interaction contract: visible
countdown, optional user interruption, automatic kernel boot on timeout. The
section 21 flag-based behavior is retained only as historical investigation
context and is explicitly superseded by this section.

## 23. Restore the complete real U-Boot log (and its real one-second delay)

Section 22 restored the external autoboot contract, but its single synthetic
countdown line did not explain a larger regression reported by the user: an
older bring-up run displayed DRAM, partition, console, network and image
loading messages, whereas the current valid-SPI run appeared to print only
the U-Boot banner before Linux. The missing text was not evidence that those
parts of U-Boot had stopped executing. It was being generated and retained in
U-Boot's own RAM logs instead of being sent directly to the modeled UART.

The behavior changed when SPI RDID and environment reads began working. The
old verbose capture used U-Boot's bad-CRC/default-environment path, whose
compiled default has `dh_keyboard=0`. The valid production `hwid` environment
has `dh_keyboard=1`. Changing only that byte in a temporary copy, while
updating the environment CRC, restored direct output but also changed the
real boot policy into a lengthy SD/TFTP recovery flow. Therefore neither the
dump nor `dh_keyboard` may be patched merely to obtain prettier output.

A live GDB comparison at `0xA0811B50` (entry to the standard countdown) found
the functional mode byte at `0xA0849F64`: it is 1 for the production path and
0 for the default path. With mode 1, the genuine messages are present in two
RAM areas:

```text
early mode/length/data: 0xA0849F64 / 0xA0849F68 / 0xA0849F6C
main log descriptor:    0xA083E4B0
descriptor +0x18:       capacity (0x4000)
descriptor +0x1c:       buffer pointer (observed 0xA0380008)
descriptor +0x20:       current write position
```

At the countdown breakpoint, the main position was 633 bytes and began with
the real `gBootLogPtr`, partition, `bootargs`, `In/Out/Err`, `TEXT_BASE` and
SD-init messages. The early scratch area contained the real timing and
`DRAM:  64 MiB` messages. This proves the correct source is U-Boot's live
buffers, not reconstructed host strings.

The UART model now polls those exact structures every 1 ms of virtual time,
validates their mode, capacity, pointer and bounds, and sends only newly
appended bytes to the console. It starts after the private OEM `'*'` has been
consumed so the banner remains first and the default-environment path is not
duplicated. It performs one final drain when the PC leaves the U-Boot image,
capturing the genuine `Starting kernel ...`, then stops before Linux can reuse
the linked globals. The synthetic countdown from section 22 was removed: the
visible countdown is now the actual string written by this U-Boot binary.

This exposed a separate timer polarity bug. Disassembly shows
`udelay()` at `0xA081D340` calling `0xA0820A54`, which obtains time through
`0xA0820918`. That function reads timer register `0xF0C00004` and applies
`MVN`, because the hardware register is a down-counter. The model incorrectly
returned increasing microsecond ticks at that offset, so the complemented
time ran backwards and the 101 calls to `udelay(10000)` in the countdown
could finish almost immediately. Register `0x04` now returns `~ticks`, just
like the confirmed down-counter semantics already modeled for timer 1. This
makes the U-Boot-owned one-second key window real; no host sleep or forced
autoboot decision is used.

Validation used the unmodified 8 MiB SPI dump and the public runner. In an
allocated TTY, the test waited until the real `Hit any key to stop autoboot:`
became visible and only then sent `x`; U-Boot responded with `disable wdt` and
its real `>` prompt, with no `Starting kernel` or `BogoMIPS`. The first
non-TTY automation attempt was correctly rejected as a test-harness error:
`docker -i` left the host pseudo-terminal in canonical mode and retained `x`
until newline, while `run.sh` uses `docker -it` for a real interactive
terminal. With no input, the other test showed the complete U-Boot log,
`Starting kernel`, Linux `BogoMIPS`, and
`VFS: Mounted root (squashfs filesystem)`, then continued into vendor
userspace.

## Status (updated again, section 23)

The default full-SPI run now executes the production environment unchanged,
displays the complete genuine U-Boot boot log, provides the real timed
keypress window and automatically reaches kernel/rootfs when no key is
pressed. Sections 21 and 22 remain the investigation history; their synthetic
countdown implementation is superseded by this section.
