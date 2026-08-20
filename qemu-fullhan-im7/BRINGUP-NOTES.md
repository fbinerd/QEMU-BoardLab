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
