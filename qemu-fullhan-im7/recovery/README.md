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

## Real-hardware failure (2026-08-20) and the bisection investigation

The first physical test of `miboim7-spi-8mb-reset-tftp-192.168.2.10.bin`
(both patches combined) did not boot: the red LED stayed weak, the green LED
never lit. Full account, including exact SHA-256 values for every dump taken
before/after, is in
`openwrt-build-tools/tools/recovery-lab/README.md`'s "Validação no EN25QH64
da IM7" section. The OEM image was restored to the physical chip immediately
after and independently reread/verified - **the device is back to its known
good state**, this was not a bricking incident.

Two facts, both confirmed by direct inspection rather than assumption, narrow
this to a real execution bug rather than a bad write:

1. `gravar_spi_ch341a.sh` reads back the chip after every write and compares
   it byte-for-byte against the source file. Both the write of the modified
   image and the later restore of the OEM image passed that check, and a
   fresh readback taken *after* the observed boot failure still matched the
   modified image's SHA-256 exactly - the chip content was never in doubt,
   never partially written, never corrupted by the failed boot attempt.
2. The 261 changed bytes on the real chip (`0x14518`-`0x15683`, file offset
   within the U-Boot partition) match **exactly** the two regions
   `patch-uboot.py` intentionally touches - the recovery cave at
   `RECOVERY_ADDRESS` (`0xA0812518`) and the single hooked instruction at
   `SECURITY_VERIFY_ADDRESS` (`0xA0813680`). Nothing was changed outside what
   the patch script itself claims to change.

Live disassembly of both patched sites (via a Python/Capstone pass over the
real `0_U-Boot.bin`, not just reading the patch script) found the trampoline
logic self-consistent on paper: correct ARM branch-offset arithmetic in both
directions, no register clobbering before the displaced original instruction
replays, and the recovery cave's own literal-pool boundary (`0xA0812630`)
respected. It did **not** find an obvious bug. The two patched regions differ
in one structurally important way, though: the recovery cave at
`RECOVERY_ADDRESS` is disassembly-confirmed to be reachable *only* when GPIO
23 reads active-low (the original OEM `beq` before it already gates entry) -
so a normal boot with the button released should never execute a single byte
of that patch - while the security-verifier hook at `SECURITY_VERIFY_ADDRESS`
is unconditional and runs on every boot regardless of button state. That
asymmetry is exactly what a bisection test can isolate.

### Bisection variants

`patch-uboot-bisect.py` builds two single-variable images from the same
verified source `0_U-Boot.bin`:

- `--mode verify-only`: hooks *only* `SECURITY_VERIFY_ADDRESS`. The recovery
  cave is left 100% byte-identical to OEM. The trampoline this mode needs is
  deliberately placed far away from the cave, in confirmed virgin `0xFF`
  padding at file offset `0x48000` (well past the last real content, which
  ends at `0x40000`) - the script refuses to run if that region isn't
  actually blank, so there's no risk of colliding with anything real.
- `--mode cave-only`: rewrites *only* the recovery cave, exactly as the full
  patch does. `SECURITY_VERIFY_ADDRESS` is left byte-identical to OEM, so the
  normal signed-kernel boot path is provably untouched by this variant.

Built outputs (2026-08-20), from the same verified source U-Boot
(`89cf6b24...49bbdf`):

```text
0_U-Boot-verify-only.bin                    sha256 158b8fdb6fe29413c8f70b32a2867de9ebbe98e2215932e3bfd139ae1079e882
0_U-Boot-cave-only.bin                      sha256 2dc4d97514edd7697e5240fe049813e1cfeb59526b0581aa767fccbe4db164a4
miboim7-spi-8mb-BISECT-verify-only.bin      sha256 7109c728a50935fe479ce3fb90596e13df9c284c61ff6c391974cfebfa5b8a26
miboim7-spi-8mb-BISECT-cave-only.bin        sha256 e5e9ee0fe2212a0ec5623338945c4ceb998aba4bfd845a0b4cad2b4b6bc9f562
```

Rebuild with:

```sh
python3 recovery/patch-uboot-bisect.py \
  /media/dados_2tb/opw/openwrt-build-tools/tools/firmware-lab/work/miboim7-tudo-sobre/particoes/0_U-Boot.bin \
  recovery/output/0_U-Boot-verify-only.bin --mode verify-only

python3 recovery/patch-uboot-bisect.py \
  /media/dados_2tb/opw/openwrt-build-tools/tools/firmware-lab/work/miboim7-tudo-sobre/particoes/0_U-Boot.bin \
  recovery/output/0_U-Boot-cave-only.bin --mode cave-only
```

Then splice each into a full 8 MiB image the same way `build-full-spi.py`
does (partition 0 replaced, `0x050000-0x7fffff` byte-identical to the OEM
dump) before writing to the physical chip.

**Suggested real-hardware test order, button released both times:**

1. Flash `miboim7-spi-8mb-BISECT-verify-only.bin`. If it boots normally,
   the verifier hook is not the cause and suspicion shifts to the cave
   mechanism itself (or something neither patch touches). If it fails the
   same way (weak red LED, no green), the verifier hook is the culprit and
   the next step is instrumenting/simplifying that trampoline specifically.
2. Flash `miboim7-spi-8mb-BISECT-cave-only.bin` (button still released).
   Given the GPIO-gating evidence above, this is expected to boot exactly
   like the OEM image - if it *doesn't*, that overturns the "cave is
   button-gated dead code on normal boot" assumption and is itself an
   important finding.
3. Restore the OEM image (`build-full-spi.py` isn't needed for this -
   the original 8 MiB dump is already the OEM image) and reread/verify
   before disconnecting the programmer, same as the first round.

One QEMU caveat worth recording so it isn't mistaken for evidence either
way: at the time of this writing, `run.sh --boot-from-spi` reboot-loops in
QEMU (repeated `Division by zero in kernel` followed by a silent reset,
never reaching `Mounted root`) for the *unmodified* OEM image too, under
non-interactive/non-TTY test conditions - a separate, already-flagged QEMU
regression unrelated to this patch (see `BRINGUP-NOTES.md`). Confirmed the
`verify-only` bisection image reproduces the identical loop in QEMU, which
is expected given the same underlying QEMU issue, not new evidence about the
real-hardware failure. QEMU is not currently a reliable gate for these two
images; the real chip is the only test that matters here.

### Real-hardware result 1: `verify-only` also fails identically

Written and verified byte-perfect (`gravar_spi_ch341a.sh`, readback SHA-256
`7109c728a50935fe479ce3fb90596e13df9c284c61ff6c391974cfebfa5b8a26`) on the
same EN25QH64, then tested with the reset button released. Result: **same
failure as the full combined patch** - weak red LED, green never lit. A
live UART monitor (`conectar_ttl.sh`, broker on the recovery-lab TCP port)
was attached before power-on and captured **zero bytes**, not even the
U-Boot banner, which prints before DRAM init and long before this patch's
touched code (`SECURITY_VERIFY_ADDRESS`, called only much later during an
actual `bootm`) would ever execute.

This is a significant update to the leading hypothesis. `verify-only`
changes only ~40 bytes total, in two small, isolated spots (4 bytes at the
hook site, file offset `0x15680`; a ~40-byte trampoline in confirmed virgin
`0xFF` padding at file offset `0x48000`, nowhere near any real code/data).
If a patch this minimal and this far from early-boot code already prevents
even the banner from reaching UART, the most likely explanation is no
longer a logic bug in either trampoline - it's an integrity check in a
stage *before* U-Boot's own code starts (boot ROM / SPL; the `"2BL*OABD"`
header magic at file offset 0 is consistent with "second bootloader,"
implying a first stage that validates it) that rejects the whole partition
on any byte difference, checksum/signature unknown. An exhaustive-ish brute
force (CRC32 with default parameters, plain word-sum LE/BE, XOR, over ~20
candidate start/end combinations spanning the header, reloc table and code
region) found no matching field in the header - inconclusive, not a
disproof; the real algorithm may use different CRC parameters, a seed, or
be a cryptographic signature that isn't brute-forceable from the ciphertext
side at all.

### Real-hardware result 2: `cave-only` also fails identically - conclusive

Written and verified byte-perfect (readback SHA-256
`e5e9ee0fe2212a0ec5623338945c4ceb998aba4bfd845a0b4cad2b4b6bc9f562`), tested
with the reset button released. **Same failure again**: weak red LED, green
never lit, zero bytes on a live UART monitor.

This is the decisive result, not just another data point. `cave-only`
touches *only* the recovery cave at `RECOVERY_ADDRESS` - code that is
disassembly-confirmed (section above, and the original patch-uboot.py's own
comment) to be reachable *exclusively* when GPIO 23 reads active-low. With
the button released, the CPU never executes a single one of those changed
bytes - the original OEM `beq` before the cave skips over it unconditionally
on this exact boot path. A runtime logic bug in code that never runs cannot
produce a symptom. Something is validating the *content* of the partition
before ever executing it, not just failing on a control-flow path we
patched.

**Conclusion**: all three tested images (full patch, verify-only, cave-only)
fail identically and silently (no UART output at all, not even the banner)
on real hardware, while all three run correctly in QEMU (which loads bytes
directly with no integrity check of any kind). The common factor across all
three is simply "the U-Boot partition's bytes differ from the OEM original."
The most consistent explanation is an integrity check - checksum or,
more likely given the `"2BL*OABD"`/second-bootloader naming convention, a
cryptographic signature - validated by a stage before U-Boot's own code
starts (mask ROM or SPL), which this repository has no visibility into and
which QEMU does not model at all (it loads the image unconditionally, see
`board/im7cam.c`'s own `rom_add_blob_fixed()` call). The exact algorithm was
not identified by brute-forcing common CRC32/checksum variants over the
header.

**Practical implication**: in-place binary patching of this U-Boot partition
is not a viable recovery-boot strategy on this specific device as currently
understood. Any single-byte change anywhere in the 320 KB partition, whether
executed or not, is rejected before boot. Making the reset-button-to-TFTP
feature work would require either (a) identifying and correctly satisfying
whatever the pre-U-Boot stage validates - hard to do without the vendor's
signing key if it *is* a signature, not just a checksum - or (b) a different
mechanism entirely that doesn't modify the validated partition, e.g. relying
on U-Boot's *own*, already-present, unmodified SD-card update path (the
exact code the cave patch replaced) instead of hijacking it, or using the
already-confirmed-working interactive TFTP path (the OEM `'*'` unlock +
manual `tftpboot`/`bootm`, proven in section "The hidden OEM U-Boot console"
of `BRINGUP-NOTES.md`) as a manual recovery procedure instead of an
automatic GPIO-triggered one.

The OEM image was restored to the physical chip immediately after this
result and independently reread/verified - the device is back to its known
good, original state.

### Confirmed: it is a signature check, not a checksum (2026-08-20)

Separately, `/media/dados_2tb/opipc` (a from-scratch, modern GPL U-Boot
2026.07 port for this board, built by a different tool the user runs -
not a patch of the vendor binary at all) was also tested. Its first build
targeted the wrong CPU (Cortex-A7/ARMv7-A; assumed `BPIALL` etc.) and hung
identically for an unrelated, independently-confirmed reason (undefined
instruction on ARMv6/ARM1176, the CPU this device's own extracted Linux
kernel `proc_info` table actually identifies - see
`arm-selfmod-lab/qemu-fullhan-im7/BRINGUP-NOTES.md` section 13d). Once
retargeted to ARM1176JZF-S/ARMv6, that from-scratch U-Boot boots correctly
in QEMU (real banner, correct CPU string, DRAM, UART, reaches the autoboot
countdown) - but real hardware researched by that same tool independently
confirmed the actual mechanism blocking every one of this repo's binary
patches: **the SoC's boot ROM verifies the U-Boot partition's signature
before executing it.** Not a checksum this repo could have brute-forced or
reverse engineered around - a real cryptographic signature check, which
requires the vendor's (Dahua/Fullhan's) private signing key to satisfy.

This closes off *both* remaining strategies from this file's earlier
"practical implication" paragraph: neither patching the existing signed
U-Boot binary nor replacing it with an unsigned from-scratch build can
boot on real hardware, regardless of how correct the code inside either
one is. The only paths that remain viable on this device are ones that
don't require the boot ROM to accept a different U-Boot at all - the
already-confirmed-working interactive TFTP recovery (OEM `'*'` console
unlock + manual `tftpboot`/`bootm`, no U-Boot modification needed), or
some other entry point below the boot ROM's own verification (out of
scope for anything discovered so far).
