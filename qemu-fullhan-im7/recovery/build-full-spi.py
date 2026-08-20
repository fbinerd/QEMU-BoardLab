#!/usr/bin/env python3

"""Replace only the U-Boot partition in the known OEM 8 MiB SPI dump."""

import argparse
import hashlib
from pathlib import Path


SPI_SIZE = 8 * 1024 * 1024
UBOOT_SIZE = 0x50000
OEM_SPI_SHA256 = "5ba8b90f01175b9344bd6fef199af3fb73e1b59d5f6bd6ebc1903a3ebfc5e295"
RECOVERY_UBOOT_SHA256 = "76387ae0165bc0ded4b5ad611936172656da2e50ca0d2af67fe39df0e5b253ad"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("oem_spi", type=Path)
    parser.add_argument("recovery_uboot", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    oem_spi = args.oem_spi.read_bytes()
    recovery_uboot = args.recovery_uboot.read_bytes()
    oem_digest = digest(oem_spi)
    uboot_digest = digest(recovery_uboot)

    if len(oem_spi) != SPI_SIZE or oem_digest != OEM_SPI_SHA256:
        raise SystemExit(
            "refusing unknown SPI dump: "
            f"size={len(oem_spi)} sha256={oem_digest}"
        )
    if len(recovery_uboot) != UBOOT_SIZE or uboot_digest != RECOVERY_UBOOT_SHA256:
        raise SystemExit(
            "refusing unknown recovery U-Boot: "
            f"size={len(recovery_uboot)} sha256={uboot_digest}"
        )

    # Partition 0 occupies [0x000000, 0x050000). Everything after it must
    # remain byte-for-byte identical to the known OEM dump.
    full_spi = recovery_uboot + oem_spi[UBOOT_SIZE:]
    if len(full_spi) != SPI_SIZE or full_spi[UBOOT_SIZE:] != oem_spi[UBOOT_SIZE:]:
        raise SystemExit("internal error: data outside U-Boot was modified")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(full_spi)
    output_digest = digest(full_spi)

    print(f"OEM SPI sha256:      {oem_digest}")
    print(f"recovery U-Boot:    {uboot_digest}")
    print(f"full recovery SPI:  {output_digest}")
    print("changed range:      0x000000-0x04ffff (U-Boot partition only)")
    print("preserved range:    0x050000-0x7fffff (identical to OEM)")
    print(f"output:             {args.output}")


if __name__ == "__main__":
    main()
