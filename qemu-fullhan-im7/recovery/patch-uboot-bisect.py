#!/usr/bin/env python3

"""Bisection variants of patch-uboot.py to isolate the real-hardware boot
failure documented in openwrt-build-tools/tools/recovery-lab/README.md
("first physical test": weak red LED, green never lit, real hardware never
booted, while the same image reaches Linux userspace in QEMU).

Byte-for-byte confirmed against the physical chip (see that README): the
patched image differs from the OEM dump in exactly 261 bytes, all within
file offsets 0x14518-0x15683 - precisely the union of the two regions
patch-uboot.py touches:
  1. the recovery cave at RECOVERY_ADDRESS (0xA0812518), GPIO23-gated,
     confirmed by disassembly to be unreachable when the reset button is
     released (the original `beq` only enters it for active-low GPIO23);
  2. the security-verifier hook at SECURITY_VERIFY_ADDRESS (0xA0813680),
     which runs on *every* boot, button state irrelevant.

Since (1) is disassembly-confirmed dead code on a normal boot, and (2) is
not, this script builds two single-variable images so a physical test with
the button released can isolate which change (if either alone) is
sufficient to reproduce the failure. To keep the two variants genuinely
independent, the verify-only trampoline is placed in confirmed 0xFF-padded
space near the end of the 320 KB partition (file offset 0x48000, well past
the last real content at 0x40000 and nowhere near the recovery cave), not
inside the cave region.
"""

import argparse
import hashlib
import struct
from pathlib import Path


EXPECTED_SHA256 = "89cf6b2401666d4af1e1cc23e5e4408d43afa9fff0fcc9c07c5662964749bbdf"
IMAGE_SIZE = 0x50000
HEADER_SKIP = 0x2000
LOAD_ADDRESS = 0xA0800000
HOOK_ADDRESS = 0xA081250C
HOOK_ORIGINAL = 0x0A000001
SECURITY_VERIFY_ADDRESS = 0xA0813680
SECURITY_VERIFY_ORIGINAL = 0xE92D42F0
RECOVERY_ADDRESS = 0xA0812518
RECOVERY_FILE_OFFSET = RECOVERY_ADDRESS - LOAD_ADDRESS + HEADER_SKIP
RECOVERY_LIMIT_ADDRESS = 0xA0812630
RUN_COMMAND = 0xA0811838
FAIL_RETURN = 0xA0812510
RECOVERY_COMMAND = (
    "setenv ipaddr 192.168.2.108;"
    "setenv serverip 192.168.2.10;"
    "setenv bootargs mem=38M console=ttyS0,115200 rdinit=/init;"
    "tftpboot 0 im7-recovery-initramfs.uImage;"
    "bootm 0xA1000000"
)

# Confirmed 0xFF-padded, far from any real code/data (last real content
# ends at file offset 0x40000 - see BRINGUP-NOTES.md / this repo's own
# investigation notes). Used only by --mode verify-only, kept completely
# separate from the GPIO23-gated recovery cave.
SAFE_PAD_FILE_OFFSET = 0x48000
SAFE_PAD_ADDRESS = SAFE_PAD_FILE_OFFSET - HEADER_SKIP + LOAD_ADDRESS


def branch(source: int, target: int, link: bool = False,
           condition: int = 0xE) -> int:
    delta = target - (source + 8)
    if delta % 4:
        raise ValueError("unaligned ARM branch")
    immediate = delta // 4
    if not -(1 << 23) <= immediate < (1 << 23):
        raise ValueError("ARM branch target is out of range")
    opcode = 0x0B000000 if link else 0x0A000000
    return (condition << 28) | opcode | (immediate & 0xFFFFFF)


def runtime_to_file(address: int) -> int:
    return address - LOAD_ADDRESS + HEADER_SKIP


def build_cave_stub():
    """Same recovery-cave stub as patch-uboot.py, unchanged."""
    command_address = RECOVERY_ADDRESS + 48
    command_bytes = RECOVERY_COMMAND.encode("ascii") + b"\0"
    flag_address = (command_address + len(command_bytes) + 3) & ~3
    verify_cave_address = flag_address + 4
    words = (
        0xE59F0020, 0xE3A01000, 0xE59F201C, 0xE3A03001, 0xE5823000,
        branch(RECOVERY_ADDRESS + 20, RUN_COMMAND, link=True),
        0xE3A03000, 0xE59F2008, 0xE5823000,
        branch(RECOVERY_ADDRESS + 36, FAIL_RETURN),
        command_address, flag_address,
    )
    stub = b"".join(struct.pack("<I", w) for w in words) + command_bytes
    stub += b"\0" * (flag_address - (RECOVERY_ADDRESS + len(stub)))
    stub += struct.pack("<I", 0)
    verify_words = (
        0xE59FC018, 0xE59CC000, 0xE35C0000,
        branch(verify_cave_address + 12, verify_cave_address + 24,
               condition=0x1),
        SECURITY_VERIFY_ORIGINAL,
        branch(verify_cave_address + 20, SECURITY_VERIFY_ADDRESS + 4),
        0xE3A00000, 0xE12FFF1E, flag_address,
    )
    stub += b"".join(struct.pack("<I", w) for w in verify_words)
    if RECOVERY_ADDRESS + len(stub) > RECOVERY_LIMIT_ADDRESS:
        raise SystemExit("recovery stub overlaps the original literal pool")
    return stub, verify_cave_address, flag_address


def build_standalone_verify_trampoline():
    """Verify-only trampoline living in safe 0xFF padding, far from the
    cave. Its own flag word is never set (no code path sets it in this
    variant), so it always takes the normal-verified branch - this variant
    exists purely to test whether the *hook mechanism itself* (branch
    redirect + displaced-instruction replay) survives on real hardware,
    independent of the recovery/TFTP logic."""
    flag_address = SAFE_PAD_ADDRESS + 0x24
    verify_cave_address = SAFE_PAD_ADDRESS
    verify_words = (
        0xE59FC018, 0xE59CC000, 0xE35C0000,
        branch(verify_cave_address + 12, verify_cave_address + 24,
               condition=0x1),
        SECURITY_VERIFY_ORIGINAL,
        branch(verify_cave_address + 20, SECURITY_VERIFY_ADDRESS + 4),
        0xE3A00000, 0xE12FFF1E, flag_address,
    )
    stub = b"".join(struct.pack("<I", w) for w in verify_words)
    stub += struct.pack("<I", 0)  # the never-set flag itself
    return stub, verify_cave_address


def load_source(path: Path) -> bytearray:
    image = bytearray(path.read_bytes())
    digest = hashlib.sha256(image).hexdigest()
    if len(image) != IMAGE_SIZE or digest != EXPECTED_SHA256:
        raise SystemExit(
            f"refusing unknown U-Boot: size={len(image)} sha256={digest}"
        )
    hook_offset = runtime_to_file(HOOK_ADDRESS)
    if struct.unpack_from("<I", image, hook_offset)[0] != HOOK_ORIGINAL:
        raise SystemExit("hook mismatch - source does not match expectation")
    verify_offset = runtime_to_file(SECURITY_VERIFY_ADDRESS)
    if struct.unpack_from("<I", image, verify_offset)[0] != SECURITY_VERIFY_ORIGINAL:
        raise SystemExit("security hook mismatch - source does not match expectation")
    return image


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--mode", choices=["verify-only", "cave-only"], required=True)
    args = parser.parse_args()

    image = load_source(args.source)
    verify_offset = runtime_to_file(SECURITY_VERIFY_ADDRESS)

    if args.mode == "cave-only":
        stub, _, _ = build_cave_stub()
        recovery_end = RECOVERY_FILE_OFFSET + len(stub)
        image[RECOVERY_FILE_OFFSET:recovery_end] = stub
        print("cave-only: recovery cave patched, security verifier UNTOUCHED")
        print(f"  cave bytes changed: file 0x{RECOVERY_FILE_OFFSET:x}-0x{recovery_end:x}")
    else:
        stub, verify_cave_address = build_standalone_verify_trampoline()
        pad_end = SAFE_PAD_FILE_OFFSET + len(stub)
        # sanity: region must be virgin 0xFF padding
        if any(b != 0xFF for b in image[SAFE_PAD_FILE_OFFSET:pad_end]):
            raise SystemExit("safe pad region is not virgin 0xFF - refusing")
        image[SAFE_PAD_FILE_OFFSET:pad_end] = stub
        struct.pack_into(
            "<I", image, verify_offset,
            branch(SECURITY_VERIFY_ADDRESS, verify_cave_address),
        )
        print("verify-only: security verifier hooked, recovery cave UNTOUCHED")
        print(f"  trampoline placed at file 0x{SAFE_PAD_FILE_OFFSET:x}-0x{pad_end:x}")
        print(f"  verifier word patched at file 0x{verify_offset:x}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    print(f"output sha256: {hashlib.sha256(image).hexdigest()}")
    print(f"output: {args.output}")


if __name__ == "__main__":
    main()
