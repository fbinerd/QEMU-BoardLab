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

## Status

**Boots into real code, doesn't reach a device yet.** Concretely:
image format (section 1), UART address (section 2, medium-high
confidence), and now also CPU core (section 3 — upgraded from a guess to
`cortex-a7`, confirmed ARMv7-A-class by a real VBAR fault, though the
*exact* core within that class is still unconfirmed) all have real
evidence behind them, not blind guesses, matching this project's standard.

Next steps, in order of how cheap they are to try:

1. **Longer run + `-d in_asm` or a GDB session** (`gdb-multiarch` is
   already in this board's own image) attached to the QEMU gdbstub to see
   where the PC actually is after attempt 3's 10s window — settles
   "looping" vs. "still making forward progress slowly" directly instead
   of guessing from silence.
2. If looping in RAM: the relocation table (section 1) is the next
   suspect — if U-Boot's own startup code tries to walk/apply it and the
   `0xed000000`-based addressing scheme means something this skeleton
   isn't providing (e.g., expects that table's addresses to *also* be
   accessible somewhere, not just be data), that would hang without ever
   touching a peripheral.
3. If looping on a genuinely-unmapped read: flip
   `ignore_memory_transaction_failures` off temporarily to get a hard
   fault with an address instead of silence — trades "boots further"
   (real hardware/firmware tolerates aborts less gracefully when this is
   off) for "tells you exactly where," useful as a one-shot diagnostic
   even if left on normally.
4. Confirm/refine `IM7CAM_RAM_SIZE` (currently a placeholder 64 MiB guess)
   and `IM7CAM_RAM_BASE`'s exact whole-file-vs-header-skipped loading
   question (still open per section 1) if the above point at either.
