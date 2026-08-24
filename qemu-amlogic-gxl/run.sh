#!/bin/bash
# Boot the MXQ Pro / GXLX p271 Android image from an emulated eMMC.
# The machine starts at the BL31 -> BL33 handoff and runs mainline U-Boot;
# the original image's BL33 cannot be extracted because its FIP is encrypted.

set -euo pipefail

IMAGE="gxl-p211-qemu:9.1.0"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EMMC_IMAGE=""
TRACE=0
STOP_AUTOBOOT=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --emmc-image) EMMC_IMAGE="$2"; shift 2 ;;
        --trace) TRACE=1; shift ;;
        --stop-autoboot) STOP_AUTOBOOT=1; shift ;;
        *) echo "usage: $0 [--emmc-image PATH] [--trace] [--stop-autoboot]" >&2; exit 2 ;;
    esac
done

if [[ -z "$EMMC_IMAGE" ]]; then
    LOCAL_IMAGE="${SCRIPT_DIR}/images/emmc_full.img"
    FACTORY_IMAGE="/media/dados_2tb/android/boards/gxl_p211_v1/reverse-engineering/full_firmware_mxqpro/emmc_full.img"
    if [[ -f "$LOCAL_IMAGE" ]]; then
        EMMC_IMAGE="$LOCAL_IMAGE"
    else
        EMMC_IMAGE="$FACTORY_IMAGE"
    fi
fi

if [[ ! -f "$EMMC_IMAGE" ]]; then
    echo "error: eMMC image not found: $EMMC_IMAGE" >&2
    exit 1
fi

FACTORY_DTB="/media/dados_2tb/android/target/linux/amlogic/gxlx/gxl_p211_v1/prebuilts/gxlx-p271-1g-base.dtb"
DTB_OVERLAY="${SCRIPT_DIR}/board/qemu-disable-secure.dts"
DTB="${SCRIPT_DIR}/out/gxlx-p271-1g-qemu.dtb"
if [[ ! -f "$FACTORY_DTB" ]]; then
    echo "error: decoded factory DTB not found: $FACTORY_DTB" >&2
    exit 1
fi
if ! command -v dtc >/dev/null 2>&1 || ! command -v fdtoverlay >/dev/null 2>&1; then
    echo "error: dtc and fdtoverlay are required to prepare the QEMU DTB" >&2
    exit 1
fi

mkdir -p "${SCRIPT_DIR}/out"
if [[ ! -f "$DTB" || "$FACTORY_DTB" -nt "$DTB" || "$DTB_OVERLAY" -nt "$DTB" ]]; then
    DTBO="${SCRIPT_DIR}/out/qemu-disable-secure.dtbo"
    dtc -@ -I dts -O dtb -o "$DTBO" "$DTB_OVERLAY"
    fdtoverlay -i "$FACTORY_DTB" -o "$DTB" "$DTBO"
fi

if ! command -v qemu-img >/dev/null 2>&1; then
    echo "error: qemu-img is required to create the disposable eMMC overlay" >&2
    exit 1
fi

QEMU_VENDOR="${SCRIPT_DIR}/vendor/qemu-9.1.0.tar.xz"
if [[ ! -f "$QEMU_VENDOR" ]]; then
    mkdir -p "${SCRIPT_DIR}/vendor"
    cp --reflink=auto "${SCRIPT_DIR}/../qemu-fullhan-im7/vendor/qemu-9.1.0.tar.xz" "$QEMU_VENDOR"
fi

echo "Building ${IMAGE} (cached after the first run)..."
docker build -t "$IMAGE" "$SCRIPT_DIR"

EMMC_DIR="$(cd "$(dirname "$EMMC_IMAGE")" && pwd)"
EMMC_FILE="$(basename "$EMMC_IMAGE")"
DTB_DIR="$(cd "$(dirname "$DTB")" && pwd)"
DTB_FILE="$(basename "$DTB")"

TRACE_ARGS=()
if [[ "$TRACE" -eq 1 ]]; then
    TRACE_ARGS=(-d unimp,guest_errors)
fi

STOP_ENV=()
if [[ "$STOP_AUTOBOOT" -eq 1 ]]; then
    STOP_ENV=(-e GXL_P211_STOP_AUTOBOOT=1)
fi

TTY_ARGS=(-i)
if [[ -t 0 && -t 1 ]]; then
    TTY_ARGS=(-it)
fi

echo "eMMC: $EMMC_IMAGE"
echo "Boot: mainline Meson GXL U-Boot -> AML boot partition at sector 0x2ae000"
echo "Console below is ttyS0. Ctrl-A X quits QEMU."
echo

OVERLAY_ROOT="${GXL_P211_TMPDIR:-${SCRIPT_DIR}/out}"
mkdir -p "$OVERLAY_ROOT"
OVERLAY_DIR="$(mktemp -d "${OVERLAY_ROOT}/emmc-overlay.XXXXXX")"
OVERLAY="${OVERLAY_DIR}/emmc.qcow2"
trap 'rm -f "$OVERLAY"; rmdir "$OVERLAY_DIR"' EXIT

# QEMU's eMMC core requires a power-of-two capacity. Present the 7.28 GiB
# factory dump through a sparse 8 GiB qcow2 overlay; the backing image stays
# read-only and only modified sectors consume temporary disk space.
qemu-img create -q -u -f qcow2 -F raw \
    -b "/emmc/${EMMC_FILE}" "$OVERLAY" 8G

docker run --rm "${TTY_ARGS[@]}" \
    -v "${EMMC_DIR}:/emmc:ro" -v "${OVERLAY_DIR}:/overlay" \
    -v "${DTB_DIR}:/dtb:ro" \
    -e "GXL_P211_DTB=/dtb/${DTB_FILE}" "${STOP_ENV[@]}" \
    "$IMAGE" \
    /build/qemu-9.1.0/build/qemu-system-aarch64 \
      -M gxl-p211 -cpu cortex-a53 -m 1G -nographic -monitor none \
      -serial stdio -drive if=sd,format=qcow2,file=/overlay/emmc.qcow2 \
      -kernel /build/u-boot/u-boot.bin "${TRACE_ARGS[@]}"
