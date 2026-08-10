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
UBI_VID_DYNAMIC = 1
UBI_VID_STATIC = 2

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


def build_vid_hdr(vol_id, lnum, data_size, used_ebs, payload,
                   vol_type=UBI_VID_STATIC):
    hdr = bytearray(64)
    struct.pack_into(">I", hdr, 0, UBI_VID_HDR_MAGIC)
    hdr[4] = 1  # version
    hdr[5] = vol_type
    hdr[6] = 0  # copy_flag
    hdr[7] = 0  # compat
    struct.pack_into(">I", hdr, 8, vol_id)
    struct.pack_into(">I", hdr, 12, lnum)
    struct.pack_into(">I", hdr, 16, 0)  # leb_ver, obsolete/reserved
    # data_size/data_crc/used_ebs are only meaningful (and only checked
    # by readers) for *static* volumes - the real U-Boot UBI reader's
    # own VID header sanity check (drivers/mtd/ubi/io.c's
    # validate_vid_hdr()) outright *rejects* a dynamic-volume LEB
    # ("bad used_ebs") unless used_ebs is exactly 0, and likewise
    # rejects a non-zero data_size/data_crc there ("non-zero
    # data_size"/"non-zero data CRC") - confirmed the hard way: writing
    # every dynamic-volume LEB's used_ebs as this volume's real LEB
    # count (matching the *static*-volume convention, and matching
    # what this field is *named*) looked reasonable and passed every
    # offline structural check, but made a real boot fail attach with
    # "UBI init error 22" on the very first LEB - real dynamic volumes
    # (confirmed by reading this exact device's own real "ubi_rootfs"
    # volume) carry data_size=0 *and* used_ebs=0 unconditionally, since
    # a dynamic volume has no UBI-level notion of "how many bytes/LEBs
    # of this volume are meaningful" at all - that's entirely up to
    # whatever filesystem is stored inside it (squashfs's own
    # superblock, in this case).
    is_static = (vol_type == UBI_VID_STATIC)
    struct.pack_into(">I", hdr, 20, data_size if is_static else 0)
    struct.pack_into(">I", hdr, 24, used_ebs if is_static else 0)
    struct.pack_into(">I", hdr, 28, 0)  # data_pad
    struct.pack_into(">I", hdr, 32,
                      ubicrc(payload) if (is_static and data_size) else 0)
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


def extract_ubi_volume(img, vol_id):
    """Reassembles a volume's raw content from its own LEBs. Full
    LEB_SIZE per LEB, not each VID header's own data_size field - real
    *dynamic* volumes (confirmed on this device's own "kernel" volume
    inside squashfs-factory.ubi, and its "ubi_rootfs" volume in a real
    captured FULL_FIRMWARE.bin) carry data_size=0 unconditionally, so
    there is no UBI-level way to know how many trailing bytes of the
    last LEB are "real" - trusting the full LEB range and letting the
    payload's own self-describing length (a FIT header's own size
    fields, a squashfs superblock's bytes_used) account for any
    trailing pad is the only generically-correct approach."""
    lebs = find_volume_lebs(img, vol_id)
    out = bytearray()
    for lnum, peb_idx, data_size in lebs:
        base = peb_idx * PEB_SIZE + DATA_OFF
        out += img[base:base + LEB_SIZE]
    return bytes(out)


def patch_volume(rootfs_ubi, vol_id, data_bytes, vol_type=UBI_VID_STATIC):
    img = bytearray(rootfs_ubi)
    nlebs = (len(data_bytes) + LEB_SIZE - 1) // LEB_SIZE

    existing = [peb for (_, peb, _) in find_volume_lebs(img, vol_id)]
    print(f"  existing volume {vol_id} PEBs: {len(existing)} "
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
        chunk = data_bytes[lnum * LEB_SIZE:(lnum + 1) * LEB_SIZE]
        data_size = len(chunk)
        payload = chunk + b"\x00" * (LEB_SIZE - data_size)

        vid_hdr = build_vid_hdr(vol_id, lnum, data_size, nlebs, chunk,
                                 vol_type=vol_type)

        base = peb_idx * PEB_SIZE
        img[base:base + 64] = template_ec_hdr
        img[base + VID_OFF:base + VID_OFF + 64] = vid_hdr
        img[base + DATA_OFF:base + DATA_OFF + LEB_SIZE] = payload

    if len(existing) > nlebs:
        # Shrinking: the trailing existing PEBs (indices nlebs..end)
        # still carry a valid VID header for this volume's *old*, now
        # too-high lnum range - if left alone, the real UBI attach
        # scanner (drivers/mtd/ubi/attach.c) finds them anyway and
        # reports a leb_count/highest_lnum that no longer matches the
        # new, smaller reserved_pebs just written to the volume table,
        # which check_av() (drivers/mtd/ubi/vtbl.c) rejects outright
        # ("UBI init error 22") - confirmed the hard way on a real
        # boot, not guessed: shrinking "ubi_rootfs" from a real
        # device's 157 PEBs down to 89 needed for a fresh squashfs
        # left 68 stale PEBs still claiming lnums 89-156. Blanking
        # just the VID header's magic (matching how an erased/unused
        # PEB looks to find_volume_lebs()/find_free_pebs() alike) is
        # enough - no need to touch the EC header or data area.
        orphaned = existing[nlebs:]
        print(f"  blanking {len(orphaned)} now-orphaned PEB(s) from the "
              f"volume's previous, larger size: {orphaned}")
        for peb_idx in orphaned:
            base = peb_idx * PEB_SIZE
            # The *whole* 64-byte VID header must read back as 0xFF, not
            # just its magic - confirmed the hard way: blanking only the
            # 4-byte magic still left this volume's *old* lnum/used_ebs/
            # data_crc/hdr_crc bytes in place, and ubi_io_read_vid_hdr()
            # (drivers/mtd/ubi/io.c) only treats a bad-magic PEB as
            # genuinely "empty" (UBI_IO_FF) if ubi_check_pattern() finds
            # the *entire* header all-0xFF; anything else with a bad
            # magic is treated as a *corrupted* header (UBI_IO_BAD_HDR)
            # instead, silently added to the attach info's corrupted-PEB
            # list - which was the real cause of a mystifying "UBI init
            # error 22" on a real boot, immediately after this exact
            # 4-byte-only version of this fix looked structurally
            # correct in every offline check.
            img[base + VID_OFF:base + VID_OFF + 64] = b"\xff" * 64

    print(f"  updating volume table: reserved_pebs -> {nlebs} "
          f"(appsbl's mini-UBI reader rejects the whole image if this "
          f"doesn't match each LEB's own used_ebs field)")
    patch_vtbl_reserved_pebs(img, vol_id, nlebs)

    return bytes(img), nlebs


def run(cmd):
    subprocess.run(cmd, check=True)


def repack_fit(tmp, kernel_bin, fdt_bin, extra_bootargs, fit_description):
    """Patches fdt_bin's chosen/bootargs-append (if extra_bootargs is
    given) and repacks kernel_bin+fdt_bin into a fresh FIT using this
    project's own required config name (FIT_CONFIG_NAME) - shared by
    both the initramfs-FIT path and the squashfs-factory path below,
    since both need the exact same treatment before appsbl's
    fit_conf_get_node() (a plain name match) will accept them."""
    if extra_bootargs:
        print("Patching DTB chosen/bootargs-append: +", repr(extra_bootargs))
        dts_path = os.path.join(tmp, "fdt.dts")
        run(["dtc", "-I", "dtb", "-O", "dts", "-o", dts_path, fdt_bin])
        with open(dts_path) as f:
            dts_text = f.read()
        # Insert right before the closing quote of the property's
        # *value* (not right after the opening quote): appsbl's
        # set_fs_bootargs() concatenates its own base cmdline
        # directly onto this property's value with no separator of
        # its own, relying entirely on the value's own leading
        # space - inserting before the opening quote instead (an
        # earlier version of this script did that) glues onto the
        # base cmdline's last word with no space at all
        # ("rootwaitmodule_blacklist=vxlan", confirmed on a real
        # boot). A leading space of our own here is added
        # defensively regardless of what follows.
        marker = 'bootargs-append = "'
        start = dts_text.index(marker) + len(marker)
        end = dts_text.index('"', start)
        dts_text = (dts_text[:end] + " " + extra_bootargs + dts_text[end:])
        with open(dts_path, "w") as f:
            f.write(dts_text)
        run(["dtc", "-I", "dts", "-O", "dtb", "-o", fdt_bin, dts_path])

    its_path = os.path.join(tmp, "mr80x-openwrt.its")
    itb_path = os.path.join(tmp, "mr80x-openwrt.itb")
    with open(its_path, "w") as f:
        f.write(ITS_TEMPLATE.format(
            kernel_gz=kernel_bin, fdt_bin=fdt_bin,
            config_name=FIT_CONFIG_NAME))
    print("Building repacked FIT with config name", FIT_CONFIG_NAME)
    run(["mkimage", "-f", its_path, itb_path])
    with open(itb_path, "rb") as f:
        new_fit = f.read()
    print(f"  new FIT: {len(new_fit)} bytes")
    return new_fit


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
                          "NOT squashfs-factory.ubi's kernel volume). "
                          "Ignored if --openwrt-squashfs-factory is given.")
    ap.add_argument("--openwrt-squashfs-factory",
                     default=None,
                     help="OpenWrt's own built "
                          "*-squashfs-factory.ubi for this target. When "
                          "given, patches BOTH the 'kernel' (vol 0) AND "
                          "'ubi_rootfs' (vol 1) UBI volumes in the "
                          "output image using this file's own real "
                          "'kernel'/'rootfs' volumes (repacking the "
                          "kernel FIT the same way as --openwrt-fit, "
                          "copying the rootfs squashfs volume as-is) - "
                          "a full, real production rootfs (procd, "
                          "opkg-installed packages, /etc/inittab, real "
                          "login) instead of the initramfs recovery/test "
                          "image's stripped-down one. Mutually exclusive "
                          "with --openwrt-fit in effect: this takes "
                          "priority when set.")
    ap.add_argument("--output",
                     default=os.path.join(repo, "images",
                                           "full_firmware_openwrt.bin"))
    ap.add_argument("--extra-bootargs",
                     default="module_blacklist=vxlan",
                     help="Appended to the DTB's chosen/bootargs-append "
                          "property (space-separated kernel cmdline "
                          "fragment, standard mainline-kernel core_param - "
                          "see load_module()'s blacklisted() check). "
                          "Default blacklists vxlan.ko, which crashes "
                          "(NULL deref in vxlan_init_net, a real struct-net "
                          "ABI mismatch between this OpenWrt tree's current "
                          "vmlinux and its stale prebuilt vxlan.ko - "
                          "confirmed via disassembly, not a board-emulation "
                          "bug - see BRINGUP-NOTES.md section 30). Pass "
                          "'' to disable.")
    args = ap.parse_args()

    required = [(args.full_firmware, "--full-firmware")]
    if args.openwrt_squashfs_factory:
        required.append((args.openwrt_squashfs_factory,
                          "--openwrt-squashfs-factory"))
    else:
        required.append((args.openwrt_fit, "--openwrt-fit"))
    for path, label in required:
        if not os.path.isfile(path):
            raise SystemExit(f"{label} not found: {path}")

    with tempfile.TemporaryDirectory() as tmp:
        print("Reading real firmware's rootfs partition "
              f"(0x{ROOTFS_PART_OFFSET:x}, 0x{ROOTFS_PART_SIZE:x} bytes)")
        with open(args.full_firmware, "rb") as f:
            full = bytearray(f.read())
        rootfs_ubi = bytes(full[ROOTFS_PART_OFFSET:
                                 ROOTFS_PART_OFFSET + ROOTFS_PART_SIZE])

        if args.openwrt_squashfs_factory:
            print("Reading OpenWrt's own squashfs-factory.ubi",
                  args.openwrt_squashfs_factory)
            with open(args.openwrt_squashfs_factory, "rb") as f:
                factory_img = bytearray(f.read())

            print("Extracting real 'kernel' (vol 0) and 'rootfs' (vol 1) "
                  "volumes from it")
            kernel_raw = extract_ubi_volume(factory_img, 0)
            rootfs_raw = extract_ubi_volume(factory_img, 1)
            print(f"  kernel volume: {len(kernel_raw)} bytes, "
                  f"rootfs volume: {len(rootfs_raw)} bytes")

            kernel_raw_path = os.path.join(tmp, "kernel_raw.itb")
            with open(kernel_raw_path, "wb") as f:
                f.write(kernel_raw)
            kernel_gz = os.path.join(tmp, "kernel.gz")
            fdt_bin = os.path.join(tmp, "fdt.bin")
            run(["dumpimage", "-T", "flat_dt", "-p", "0", "-o", kernel_gz,
                 kernel_raw_path])
            run(["dumpimage", "-T", "flat_dt", "-p", "1", "-o", fdt_bin,
                 kernel_raw_path])

            new_fit = repack_fit(tmp, kernel_gz, fdt_bin,
                                  args.extra_bootargs,
                                  "ARM64 OpenWrt FIT (repacked from "
                                  "squashfs-factory.ubi)")

            print("Patching the 'kernel' (static) and 'ubi_rootfs' "
                  "(dynamic) UBI volumes in place - everything else "
                  "(env, appsbl itself, rootfs_data/overlay) untouched")
            rootfs_ubi_bytes, nlebs_k = patch_volume(
                rootfs_ubi, 0, new_fit, vol_type=UBI_VID_STATIC)
            print(f"  wrote {nlebs_k} LEBs for the new kernel volume")
            rootfs_ubi_bytes, nlebs_r = patch_volume(
                rootfs_ubi_bytes, 1, rootfs_raw, vol_type=UBI_VID_DYNAMIC)
            print(f"  wrote {nlebs_r} LEBs for the new ubi_rootfs volume")
            new_rootfs_ubi = rootfs_ubi_bytes
        else:
            print("Extracting kernel-1/fdt-1 from OpenWrt's own "
                  "initramfs FIT", args.openwrt_fit)
            kernel_gz = os.path.join(tmp, "kernel.gz")
            fdt_bin = os.path.join(tmp, "fdt.bin")
            run(["dumpimage", "-T", "flat_dt", "-p", "0", "-o", kernel_gz,
                 args.openwrt_fit])
            run(["dumpimage", "-T", "flat_dt", "-p", "1", "-o", fdt_bin,
                 args.openwrt_fit])

            new_fit = repack_fit(tmp, kernel_gz, fdt_bin,
                                  args.extra_bootargs,
                                  "ARM64 OpenWrt FIT (repacked for "
                                  "mr80x-appsbl)")

            print("Patching the 'kernel' UBI volume in place "
                  "(the 'ubi_rootfs' volume and everything else is "
                  "untouched)")
            new_rootfs_ubi, nlebs = patch_volume(rootfs_ubi, 0, new_fit,
                                                  vol_type=UBI_VID_STATIC)
            print(f"  wrote {nlebs} LEBs for the new kernel volume")

        full[ROOTFS_PART_OFFSET:ROOTFS_PART_OFFSET + ROOTFS_PART_SIZE] = \
            new_rootfs_ubi

        os.makedirs(os.path.dirname(args.output), exist_ok=True)
        with open(args.output, "wb") as f:
            f.write(full)
        print("Wrote", args.output, f"({len(full)} bytes)")


if __name__ == "__main__":
    main()
