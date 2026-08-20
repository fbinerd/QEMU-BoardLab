#!/usr/bin/env python3

"""Build the QEMU research U-Boot variant with reset-button TFTP recovery."""

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


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    image = bytearray(args.source.read_bytes())
    digest = hashlib.sha256(image).hexdigest()
    if len(image) != IMAGE_SIZE or digest != EXPECTED_SHA256:
        raise SystemExit(
            "refusing unknown U-Boot: "
            f"size={len(image)} sha256={digest}"
        )

    hook_offset = runtime_to_file(HOOK_ADDRESS)
    original = struct.unpack_from("<I", image, hook_offset)[0]
    if original != HOOK_ORIGINAL:
        raise SystemExit(
            f"hook mismatch at 0x{hook_offset:x}: 0x{original:08x}"
        )
    verify_offset = runtime_to_file(SECURITY_VERIFY_ADDRESS)
    verify_original = struct.unpack_from("<I", image, verify_offset)[0]
    if verify_original != SECURITY_VERIFY_ORIGINAL:
        raise SystemExit(
            f"security hook mismatch at 0x{SECURITY_VERIFY_ADDRESS:08x}: "
            f"0x{verify_original:08x}"
        )
    # The original BEQ at A081250C already reaches A0812518 only for an
    # active-low GPIO 23. Replace that obsolete SD-update body in place.
    command_address = RECOVERY_ADDRESS + 48
    command_bytes = RECOVERY_COMMAND.encode("ascii") + b"\0"
    flag_address = (command_address + len(command_bytes) + 3) & ~3
    verify_cave_address = flag_address + 4
    words = (
        0xE59F0020,  # ldr r0, [pc, #32] -> recovery command
        0xE3A01000,  # mov r1, #0
        0xE59F201C,  # ldr r2, [pc, #28] -> recovery flag
        0xE3A03001,  # mov r3, #1
        0xE5823000,  # str r3, [r2]
        branch(RECOVERY_ADDRESS + 20, RUN_COMMAND, link=True),
        0xE3A03000,  # mov r3, #0 (bootm returned, therefore failed)
        0xE59F2008,  # ldr r2, [pc, #8] -> recovery flag
        0xE5823000,  # str r3, [r2]
        branch(RECOVERY_ADDRESS + 36, FAIL_RETURN),
        command_address,
        flag_address,
    )
    stub = b"".join(
        struct.pack("<I", word)
        for word in words
    ) + command_bytes
    stub += b"\0" * (flag_address - (RECOVERY_ADDRESS + len(stub)))
    stub += struct.pack("<I", 0)

    # The production kernel carries a vendor RSA security trailer. Recovery
    # images built from GPL sources cannot be signed with the device key. Keep
    # the OEM verifier unchanged for every normal boot, returning success only
    # while the private reset-button recovery flag is set.
    verify_words = (
        0xE59FC018,  # ldr r12, [pc, #24] -> recovery flag address
        0xE59CC000,  # ldr r12, [r12]
        0xE35C0000,  # cmp r12, #0
        branch(verify_cave_address + 12, verify_cave_address + 24,
               condition=0x1),
        SECURITY_VERIFY_ORIGINAL,  # displaced push, normal verified path
        branch(verify_cave_address + 20, SECURITY_VERIFY_ADDRESS + 4),
        0xE3A00000,  # mov r0, #0 (recovery-only success)
        0xE12FFF1E,  # bx lr
        flag_address,
    )
    stub += b"".join(struct.pack("<I", word) for word in verify_words)
    recovery_end = RECOVERY_FILE_OFFSET + len(stub)
    if RECOVERY_ADDRESS + len(stub) > RECOVERY_LIMIT_ADDRESS:
        raise SystemExit("recovery stub overlaps the original literal pool")

    image[RECOVERY_FILE_OFFSET:recovery_end] = stub
    struct.pack_into(
        "<I", image, verify_offset,
        branch(SECURITY_VERIFY_ADDRESS, verify_cave_address),
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)

    output_digest = hashlib.sha256(image).hexdigest()
    print(f"source sha256: {digest}")
    print(
        "button branch retained: "
        f"0x{HOOK_ADDRESS:08x} -> recovery body 0x{RECOVERY_ADDRESS:08x}"
    )
    print(
        "security verifier bypass: reset-button recovery only; "
        "normal boot retains the OEM verifier"
    )
    print(
        f"recovery command ({len(RECOVERY_COMMAND)} bytes): "
        f"{RECOVERY_COMMAND}"
    )
    print(f"output sha256: {output_digest}")
    print(f"output: {args.output}")


if __name__ == "__main__":
    main()
