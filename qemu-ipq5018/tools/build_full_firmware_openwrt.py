#!/usr/bin/env python3
"""Builds images/full_firmware_openwrt.bin: a copy of the real
FULL_FIRMWARE.bin dump with its "kernel" UBI volume (inside the "rootfs"
MTD partition, offset 0x640000/size 0x2A00000) replaced by a FIT image
wrapping a real, modern, fully-sourced OpenWrt AArch64 kernel+DTB for this
exact device - so appsbl's own real NAND/UBI/FIT-loading + jump_kernel64()
AArch32->AArch64 handoff code (BRINGUP-NOTES.md section 29) runs against a
kernel that can actually execute, instead of the vendor's original 32-bit
ARM Linux-4.4.60 build (which never calls jump_kernel64() at all - see that
section for how this was discovered).

Requires:
  - images/FULL_FIRMWARE.bin (real flash dump, gitignored, not included)
  - OpenWrt's own build output for this target, specifically the
    "initramfs-uImage.itb" FIT (already a correctly-built FIT: gzip
    Linux 6.12.94 Image with an *embedded initramfs* + real device DTB,
    load/entry 0x41000000, crc32+sha1 hashes) - NOT "squashfs-factory.ubi"'s
    own kernel volume, which expects a separate "ubi_rootfs" UBI volume
    this script does not populate; the kernel would just hang forever at
    "Waiting for root device /dev/ubiblock0_1..." (confirmed the hard way -
    see BRINGUP-NOTES.md section 29's "first attempt" note). The
    initramfs build needs nothing else: userspace is embedded in the
    kernel Image itself, same as the isolated --aarch64-openwrt test path
    (section 28) already relies on. Default path below assumes a sibling
    openwrt checkout at the location this project's other sessions used;
    override with --openwrt-fit if yours lives elsewhere.
  - `mkimage`/`dumpimage` (apt install u-boot-tools, or use OpenWrt's own
    staging_dir/host/bin copies) on PATH.

Usage:
  python3 tools/build_full_firmware_openwrt.py

Output: images/full_firmware_openwrt.bin - use with
  MR80X_NAND_IMAGE=images/full_firmware_openwrt.bin ./run.sh
or
  ./run.sh --nand-image images/full_firmware_openwrt.bin

Why this specific approach (surgical UBI patch, not a fresh ubinize):
appsbl's board_select_config() (board/qca/arm/common/cmd_bootqca.c) reads a
`config_name` property baked into appsbl's OWN compiled-in control DTB
(arch/arm/dts/ipq5018-emulation.dts: config_name = "config@emulation-c2")
and looks that up *by name* in the FIT (fit_conf_get_node(), a plain name
match - CONFIG_FIT_BEST_MATCH is not enabled in this build, so there's no
device-tree compatible-string matching to worry about). So the new FIT
only needs a config node named exactly "config@emulation-c2" - everything
else about the real flash image (env, appsbl itself, the UBI layout
volume, the other "ubi_rootfs" volume) is left completely untouched.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

UBI_EC_HDR_MAGIC = 0x55424923
UBI_VID_HDR_MAGIC = 0x55424921
UBI_LAYOUT_VOLUME_ID = 0x7FFFFFFF - 4096

PEB_SIZE = 0x20000
VID_OFF = 2048
DATA_OFF = 4096
LEB_SIZE = PEB_SIZE - DATA_OFF  # 126976

ROOTFS_PART_OFFSET = 0x640000
ROOTFS_PART_SIZE = 0x2A00000

FIT_CONFIG_NAME = "config@emulation-c2"

ITS_TEMPLATE = """/dts-v1/;

/ {{
	description = "ARM64 OpenWrt FIT (Flattened Image Tree)";
	#address-cells = <1>;

	images {{
		kernel-1 {{
			description = "ARM64 OpenWrt Linux (repacked for mr80x-appsbl)";
			data = /incbin/("{kernel_gz}");
			type = "kernel";
			arch = "arm64";
			os = "linux";
			compression = "gzip";
			load = <0x41000000>;
			entry = <0x41000000>;
			hash-1 {{ algo = "crc32"; }};
			hash-2 {{ algo = "sha1"; }};
		}};

		fdt-1 {{
			description = "ARM64 OpenWrt device tree blob";
			data = /incbin/("{fdt_bin}");
			type = "flat_dt";
			arch = "arm64";
			compression = "none";
			hash-1 {{ algo = "crc32"; }};
			hash-2 {{ algo = "sha1"; }};
		}};
	}};

	configurations {{
		default = "{config_name}";
		{config_name} {{
			description = "OpenWrt mercusys_mr80x-v5 (mr80x-appsbl {config_name} slot)";
			kernel = "kernel-1";
			fdt = "fdt-1";
		}};
	}};
}};
"""


def ubicrc(b):
    return (zlib.crc32(b) ^ 0xFFFFFFFF) & 0xFFFFFFFF


def be32(b, off):
    return struct.unpack_from(">I", b, off)[0]


def find_volume_lebs(img, vol_id):
    npebs = len(img) // PEB_SIZE
    lebs = []
    for i in range(npebs):
        peb = img[i * PEB_SIZE:(i + 1) * PEB_SIZE]
        if be32(peb, 0) != UBI_EC_HDR_MAGIC:
            continue
        vhdr_off = be32(peb, 16)
        if be32(peb, vhdr_off) != UBI_VID_HDR_MAGIC:
            continue
        if be32(peb, vhdr_off + 8) != vol_id:
            continue
        lnum = be32(peb, vhdr_off + 12)
        data_size = be32(peb, vhdr_off + 20)
        lebs.append((lnum, i, data_size))
    lebs.sort()
    return lebs


def find_free_pebs(img, count):
    npebs = len(img) // PEB_SIZE
    free = []
    for i in range(npebs):
        peb = img[i * PEB_SIZE:(i + 1) * PEB_SIZE]
        if be32(peb, 0) != UBI_EC_HDR_MAGIC:
            continue
        vhdr_off = be32(peb, 16)
        if be32(peb, vhdr_off) == UBI_VID_HDR_MAGIC:
            continue  # in use by some volume
        free.append(i)
        if len(free) >= count:
            break
    return free


def build_vid_hdr(vol_id, lnum, data_size, used_ebs, payload):
    hdr = bytearray(64)
    struct.pack_into(">I", hdr, 0, UBI_VID_HDR_MAGIC)
    hdr[4] = 1  # version
    hdr[5] = 2  # vol_type = UBI_VID_STATIC
    hdr[6] = 0  # copy_flag
    hdr[7] = 0  # compat
    struct.pack_into(">I", hdr, 8, vol_id)
    struct.pack_into(">I", hdr, 12, lnum)
    struct.pack_into(">I", hdr, 16, 0)  # leb_ver, obsolete/reserved
    struct.pack_into(">I", hdr, 20, data_size)
    struct.pack_into(">I", hdr, 24, used_ebs)
    struct.pack_into(">I", hdr, 28, 0)  # data_pad
    struct.pack_into(">I", hdr, 32, ubicrc(payload) if data_size else 0)
    struct.pack_into(">Q", hdr, 40, 0)  # sqnum, not tracked by this reader
    crc = ubicrc(bytes(hdr[0:60]))
    struct.pack_into(">I", hdr, 60, crc)
    return bytes(hdr)


VTBL_RECORD_SIZE = 172


def find_layout_volume_pebs(img):
    npebs = len(img) // PEB_SIZE
    pebs = []
    for i in range(npebs):
        peb = img[i * PEB_SIZE:(i + 1) * PEB_SIZE]
        if be32(peb, 0) != UBI_EC_HDR_MAGIC:
            continue
        vhdr_off = be32(peb, 16)
        if be32(peb, vhdr_off) != UBI_VID_HDR_MAGIC:
            continue
        if be32(peb, vhdr_off + 8) != UBI_LAYOUT_VOLUME_ID:
            continue
        doff = be32(peb, 20)
        pebs.append((i, doff))
    return pebs


def patch_vtbl_reserved_pebs(img, vol_id, new_reserved_pebs):
    """appsbl's own mini-UBI reader cross-checks each volume's VID headers
    (used_ebs) against the volume table's reserved_pebs and refuses the
    whole UBI image ("UBI init error 22") if they disagree - so growing a
    volume beyond its original reserved_pebs (as this script does for the
    "kernel" volume whenever the new FIT needs more LEBs than the original
    one used) requires patching *both* copies of the volume table's record
    for that volume, not just writing the extra LEBs themselves."""
    for peb_idx, doff in find_layout_volume_pebs(img):
        base = peb_idx * PEB_SIZE + doff + vol_id * VTBL_RECORD_SIZE
        rec = bytearray(img[base:base + VTBL_RECORD_SIZE])
        struct.pack_into(">I", rec, 0, new_reserved_pebs)
        crc = ubicrc(bytes(rec[0:VTBL_RECORD_SIZE - 4]))
        struct.pack_into(">I", rec, VTBL_RECORD_SIZE - 4, crc)
        img[base:base + VTBL_RECORD_SIZE] = rec


def patch_kernel_volume(rootfs_ubi, fit_bytes, vol_id=0):
    img = bytearray(rootfs_ubi)
    nlebs = (len(fit_bytes) + LEB_SIZE - 1) // LEB_SIZE

    existing = [peb for (_, peb, _) in find_volume_lebs(img, vol_id)]
    print(f"  existing kernel-volume PEBs: {len(existing)} "
          f"(reused for the first {min(len(existing), nlebs)} LEBs)")
    if nlebs > len(existing):
        extra_needed = nlebs - len(existing)
        extra = find_free_pebs(img, extra_needed)
        if len(extra) < extra_needed:
            raise SystemExit(
                f"not enough free PEBs: need {extra_needed} more, "
                f"found {len(extra)}")
        print(f"  borrowing {len(extra)} free PEBs for the extra LEBs: "
              f"{extra}")
        peb_plan = existing + extra
    else:
        peb_plan = existing

    template_ec_hdr = bytes(img[existing[0] * PEB_SIZE:
                                 existing[0] * PEB_SIZE + 64])

    for lnum in range(nlebs):
        peb_idx = peb_plan[lnum]
        chunk = fit_bytes[lnum * LEB_SIZE:(lnum + 1) * LEB_SIZE]
        data_size = len(chunk)
        payload = chunk + b"\x00" * (LEB_SIZE - data_size)

        vid_hdr = build_vid_hdr(vol_id, lnum, data_size, nlebs, chunk)

        base = peb_idx * PEB_SIZE
        img[base:base + 64] = template_ec_hdr
        img[base + VID_OFF:base + VID_OFF + 64] = vid_hdr
        img[base + DATA_OFF:base + DATA_OFF + LEB_SIZE] = payload

    print(f"  updating volume table: reserved_pebs -> {nlebs} "
          f"(appsbl's mini-UBI reader rejects the whole image if this "
          f"doesn't match each LEB's own used_ebs field)")
    patch_vtbl_reserved_pebs(img, vol_id, nlebs)

    return bytes(img), nlebs


def run(cmd):
    subprocess.run(cmd, check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(here)
    ap.add_argument("--full-firmware",
                     default=os.path.join(repo, "images", "FULL_FIRMWARE.bin"))
    ap.add_argument("--openwrt-fit",
                     default="/media/dados_2tb/opw/openwrt/bin/targets/"
                             "qualcommax/ipq50xx/"
                             "openwrt-qualcommax-ipq50xx-mercusys_mr80x-v5-"
                             "initramfs-uImage.itb",
                     help="OpenWrt's own built initramfs FIT for this "
                          "target (a plain .itb file, embedded rootfs - "
                          "NOT squashfs-factory.ubi's kernel volume).")
    ap.add_argument("--output",
                     default=os.path.join(repo, "images",
                                           "full_firmware_openwrt.bin"))
    args = ap.parse_args()

    for path, label in [(args.full_firmware, "--full-firmware"),
                         (args.openwrt_fit, "--openwrt-fit")]:
        if not os.path.isfile(path):
            raise SystemExit(f"{label} not found: {path}")

    with tempfile.TemporaryDirectory() as tmp:
        print("Extracting kernel-1/fdt-1 from OpenWrt's own initramfs FIT",
              args.openwrt_fit)
        kernel_gz = os.path.join(tmp, "kernel.gz")
        fdt_bin = os.path.join(tmp, "fdt.bin")
        run(["dumpimage", "-T", "flat_dt", "-p", "0", "-o", kernel_gz, args.openwrt_fit])
        run(["dumpimage", "-T", "flat_dt", "-p", "1", "-o", fdt_bin, args.openwrt_fit])

        its_path = os.path.join(tmp, "mr80x-openwrt.its")
        itb_path = os.path.join(tmp, "mr80x-openwrt.itb")
        with open(its_path, "w") as f:
            f.write(ITS_TEMPLATE.format(
                kernel_gz=kernel_gz, fdt_bin=fdt_bin,
                config_name=FIT_CONFIG_NAME))
        print("Building repacked FIT with config name", FIT_CONFIG_NAME)
        run(["mkimage", "-f", its_path, itb_path])
        with open(itb_path, "rb") as f:
            new_fit = f.read()
        print(f"  new FIT: {len(new_fit)} bytes")

        print("Reading real firmware's rootfs partition "
              f"(0x{ROOTFS_PART_OFFSET:x}, 0x{ROOTFS_PART_SIZE:x} bytes)")
        with open(args.full_firmware, "rb") as f:
            full = bytearray(f.read())
        rootfs_ubi = bytes(full[ROOTFS_PART_OFFSET:
                                 ROOTFS_PART_OFFSET + ROOTFS_PART_SIZE])

        print("Patching the 'kernel' UBI volume in place "
              "(the 'ubi_rootfs' volume and everything else is untouched)")
        new_rootfs_ubi, nlebs = patch_kernel_volume(rootfs_ubi, new_fit)
        print(f"  wrote {nlebs} LEBs for the new kernel volume")

        full[ROOTFS_PART_OFFSET:ROOTFS_PART_OFFSET + ROOTFS_PART_SIZE] = \
            new_rootfs_ubi

        os.makedirs(os.path.dirname(args.output), exist_ok=True)
        with open(args.output, "wb") as f:
            f.write(full)
        print("Wrote", args.output, f"({len(full)} bytes)")


if __name__ == "__main__":
    main()
