#!/bin/bash
# Boots the extracted im7 (Imou IPC-S21F) U-Boot partition in the im7cam
# QEMU skeleton machine. Not a finished boot yet - see BRINGUP-NOTES.md
# for exactly what's confirmed vs. still a probe (section 6 as of this
# writing: real U-Boot banner + DRAM/env/network log, real SPI-backed
# flash reads, but the JEDEC ID probe itself still reports a bogus chip).
#
# Usage:
#   ./run.sh [--spi-image PATH] [path-to-0_U-Boot.bin]
#
# With no positional argument, looks for images/0_U-Boot.bin (gitignored -
# copy it in yourself from the source investigation, see BRINGUP-NOTES.md
# "Where the firmware came from").
#
# --spi-image: backs the SPI flash controller with a real flash dump
# (mirrors qemu-ipq5018/run.sh's --nand-image) so U-Boot's own flash
# reads return real data instead of 0xFF. Auto-detected if not given:
# prefers the repository-local, gitignored images/spi-flash.bin, falls
# back to the real dump's location in the sibling openwrt-build-tools
# investigation.
#
# -d unimp: makes the logging stub's trace actually visible (LOG_UNIMP is
# silent by default). Ctrl-A X quits QEMU.

set -euo pipefail

IMAGE=im7cam-qemu:9.1.0
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SPI_IMAGE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --spi-image)
            SPI_IMAGE="$2"
            shift 2
            ;;
        *)
            KERNEL="$1"
            shift
            ;;
    esac
done

KERNEL="${KERNEL:-${SCRIPT_DIR}/images/0_U-Boot.bin}"

if [[ ! -f "$KERNEL" ]]; then
    echo "error: no U-Boot image found." >&2
    echo "usage: $0 [--spi-image PATH] [path-to-0_U-Boot.bin]" >&2
    echo "default location: ${SCRIPT_DIR}/images/0_U-Boot.bin (gitignored)" >&2
    exit 1
fi

if [[ -z "$SPI_IMAGE" ]]; then
    LOCAL_SPI_IMAGE="${SCRIPT_DIR}/images/spi-flash.bin"
    FALLBACK_SPI_IMAGE="/media/dados_2tb/opw/openwrt-build-tools/tools/firmware-lab/work/miboim7-tudo-sobre/miboim7-spi-en25qh64-8mb-20260819.bin"
    if [[ -f "$LOCAL_SPI_IMAGE" ]]; then
        SPI_IMAGE="$LOCAL_SPI_IMAGE"
        echo "Auto-detected repository-local SPI dump: ${SPI_IMAGE}"
    elif [[ -f "$FALLBACK_SPI_IMAGE" ]]; then
        SPI_IMAGE="$FALLBACK_SPI_IMAGE"
        echo "Auto-detected real SPI dump: ${SPI_IMAGE} (pass --spi-image PATH to use a different one)"
    else
        echo "No SPI dump found - flash reads will return 0xFF. Pass --spi-image PATH." >&2
    fi
fi

echo "Building ${IMAGE} (cheap after the first run - only board/im7cam.c changes invalidate the ninja step)..."
docker build -t "$IMAGE" "$SCRIPT_DIR"

KERNEL_DIR="$(cd "$(dirname "$KERNEL")" && pwd)"
KERNEL_FILE="$(basename "$KERNEL")"

DOCKER_VOLUMES=(-v "${KERNEL_DIR}:/fw:ro")
DOCKER_ENV=()
if [[ -n "$SPI_IMAGE" ]]; then
    SPI_DIR="$(cd "$(dirname "$SPI_IMAGE")" && pwd)"
    SPI_FILE="$(basename "$SPI_IMAGE")"
    DOCKER_VOLUMES+=(-v "${SPI_DIR}:/spi:ro")
    DOCKER_ENV+=(-e "IM7CAM_SPI_IMAGE=/spi/${SPI_FILE}")
fi

echo "Booting ${KERNEL} in the im7cam skeleton machine."
echo "Console below IS the UART. Ctrl-A X to quit."
echo

exec docker run --rm -it \
    "${DOCKER_VOLUMES[@]}" \
    "${DOCKER_ENV[@]}" \
    "$IMAGE" \
    /build/qemu-9.1.0/build/qemu-system-arm -M im7cam -nographic -monitor none \
    -serial stdio -d unimp \
    -kernel "/fw/${KERNEL_FILE}"
