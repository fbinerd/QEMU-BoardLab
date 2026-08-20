#!/bin/bash
# Boots the extracted im7 (Imou IPC-S21F) U-Boot partition in the im7cam
# QEMU skeleton machine. Not a finished boot yet - see BRINGUP-NOTES.md
# for exactly what's confirmed vs. still a probe (section 19 as of this
# writing: SPI DMA loads the real Linux/rootfs; the 1 MHz periodic timer,
# interrupt controller and vendor-kernel console are sufficient for visible
# Linux and vendor-userspace initialization; both unattached MMC hosts are
# reported absent and SquashFS mounts from the real flash-backed mtdblock4).
#
# Usage:
#   ./run.sh [--spi-image PATH] [--trace] [path-to-0_U-Boot.bin]
#
# With no positional argument, boots partition 0 directly from the full SPI
# dump, matching qemu-ipq5018's default full-NAND mode. A positional
# 0_U-Boot.bin remains an explicit development override.
#
# --spi-image: backs the SPI flash controller with a real flash dump
# (mirrors qemu-ipq5018/run.sh's --nand-image) so U-Boot's own flash
# reads return real data instead of 0xFF. Auto-detected if not given:
# prefers the repository-local, gitignored images/spi-flash.bin, falls
# back to the real dump's location in the sibling openwrt-build-tools
# investigation.
#
# --trace: turns on `-d unimp` (LOG_UNIMP, silent by default) to see
# every access an unmodeled register gets - the tool this whole board's
# bringup method depends on (see BRINGUP-NOTES.md), but NOT the default
# here anymore: several of the guest's own real init loops retry a
# register hundreds of times in a row before giving up and moving on
# (harmless, self-resolving, a few hundred ms of real boot time) - with
# --trace on, that's hundreds of log lines per retry loop flooding an
# interactive terminal, which reads as a hang even though the emulator
# itself is fine. Use --trace only when actually hunting a new register,
# same as this file's own bringup sessions did - not for normal use.
# Ctrl-A X quits QEMU.

set -euo pipefail

IMAGE=im7cam-qemu:9.1.0
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SPI_IMAGE=""
TRACE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --spi-image)
            SPI_IMAGE="$2"
            shift 2
            ;;
        --trace)
            TRACE=1
            shift
            ;;
        *)
            KERNEL="$1"
            shift
            ;;
    esac
done

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

if [[ -n "${KERNEL:-}" && ! -f "$KERNEL" ]]; then
    echo "error: U-Boot development override not found: ${KERNEL}" >&2
    exit 1
fi
if [[ -z "${KERNEL:-}" && -z "$SPI_IMAGE" ]]; then
    echo "error: no full SPI image or U-Boot development override found." >&2
    echo "usage: $0 [--spi-image PATH] [path-to-0_U-Boot.bin]" >&2
    exit 1
fi

echo "Building ${IMAGE} (cheap after the first run - only board/im7cam.c changes invalidate the ninja step)..."
docker build -t "$IMAGE" "$SCRIPT_DIR"

DOCKER_VOLUMES=()
DOCKER_ENV=()
QEMU_BOOT_ARGS=()
if [[ -n "${KERNEL:-}" ]]; then
    KERNEL_DIR="$(cd "$(dirname "$KERNEL")" && pwd)"
    KERNEL_FILE="$(basename "$KERNEL")"
    DOCKER_VOLUMES+=(-v "${KERNEL_DIR}:/fw:ro")
    QEMU_BOOT_ARGS+=(-kernel "/fw/${KERNEL_FILE}")
    echo "Development override: booting U-Boot from ${KERNEL}"
else
    echo "Boot source: U-Boot partition inside ${SPI_IMAGE}"
fi
if [[ -n "$SPI_IMAGE" ]]; then
    SPI_DIR="$(cd "$(dirname "$SPI_IMAGE")" && pwd)"
    SPI_FILE="$(basename "$SPI_IMAGE")"
    DOCKER_VOLUMES+=(-v "${SPI_DIR}:/spi:ro")
    DOCKER_ENV+=(-e "IM7CAM_SPI_IMAGE=/spi/${SPI_FILE}")
fi

QEMU_TRACE_ARGS=()
if [[ "$TRACE" -eq 1 ]]; then
    QEMU_TRACE_ARGS=(-d unimp)
    echo "Tracing on (-d unimp) - expect a lot of retry-loop noise, see this script's own comments."
fi

echo "Booting the im7cam machine."
echo "Console below IS the UART. Ctrl-A X to quit."
echo

DOCKER_STDIN_ARGS=(-i)
if [[ -t 0 && -t 1 ]]; then
    DOCKER_STDIN_ARGS=(-it)
fi

exec docker run --rm "${DOCKER_STDIN_ARGS[@]}" \
    "${DOCKER_VOLUMES[@]}" \
    "${DOCKER_ENV[@]}" \
    "$IMAGE" \
    /build/qemu-9.1.0/build/qemu-system-arm -M im7cam -nographic -monitor none \
    -serial stdio "${QEMU_TRACE_ARGS[@]}" \
    "${QEMU_BOOT_ARGS[@]}"
