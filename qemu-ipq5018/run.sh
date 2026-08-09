#!/bin/bash
# Boots a real appsbl ELF in the mr80x QEMU machine, with interactive
# UART console (same as connecting a TTL adapter to the real router)
# and, unless --no-net is given, network access (same as plugging an
# Ethernet cable into the router's LAN port) with the recovery HTTP
# server reachable from the host.
#
# Usage:
#   ./run.sh [options] [path-to-development-appsbl.elf-or-.bin]
#
# Examples:
#   ./run.sh                         # boot APPSBL from auto-detected full NAND
#   ./run.sh --nand-image /path/to/FULL_FIRMWARE.bin
#   ./run.sh /media/dados_2tb/appsbl/out/appsbl.unpadded.elf
#   ./run.sh --recovery /media/dados_2tb/appsbl/out/appsbl.unpadded.elf
#   ./run.sh --http-port 9090 /media/dados_2tb/appsbl/out/appsbl.unpadded.elf
#   ./run.sh --nand-image /path/to/FULL_FIRMWARE.bin /media/dados_2tb/appsbl/out/appsbl.unpadded.elf
#   ./run.sh --stop-autoboot /media/dados_2tb/appsbl/out/appsbl.unpadded.elf
#
# --stop-autoboot: land straight in the interactive u-boot console
# instead of racing the "Hit any key to stop autoboot" countdown.
# Recommended if you actually want the console - that countdown is
# only ~1 real second on this build (CONFIG_BOOTDELAY=1) and terminal
# -> docker -> container -> QEMU input latency reliably eats the
# whole window before a real keypress can land, even though the timer
# itself is real-time-accurate. This works by pre-seeding the UART's
# receive buffer before the guest ever runs, so u-boot's own very
# first "is a key waiting?" check (which happens instantly, no delay)
# already sees one - zero timing dependency, unlike an actual keypress.
# With no positional APPSBL argument, the board boots the real APPSBL partition
# directly from --nand-image (or the auto-detected FULL_FIRMWARE.bin), modeling
# the final SBL/QSEE handoff. A positional ELF/bin remains an explicit
# development override while the same full image continues backing the NAND.
#
# --nand-image: back real QPIC NAND page reads with a raw full-flash
# dump (BRINGUP-NOTES.md section 4b/17b) instead of returning 0xFF for
# every page. Auto-detected if not given (see DEFAULT_NAND_IMAGE
# below) - only needed explicitly if your dump lives somewhere else.
# Without any real image, NAND device *identification* still works
# (real driver code path, genuine ID match) but there's no real data
# behind it - readenv() and any real kernel/rootfs load will always
# fail. With one, real partition data becomes readable (confirmed:
# rootfs's real UBI header reads back correctly, and `smeminfo` at the
# console lists all 16 real partitions, section 20) - but note the
# *env* partition specifically is genuinely blank in the one capture
# used for this project so far, see section 17b - that "bad CRC"
# warning is not a bug either flag fixes.
#
# Console: this terminal IS the TTL/UART connection - type at the
# prompt exactly like you would over a real serial adapter. Ctrl-A X
# quits QEMU.
#
# Network: with networking on (the default), the router boots exactly
# as real hardware would - normal boot unless --recovery is given
# (which models holding the reset button, same as on real hardware).
# The recovery HTTP server, once reached, is at http://localhost:<http-port>/.
#
# --guest-ip: the router's own LAN-side IP address inside the
# emulator. Defaults to 192.168.1.1 because appsbl's *compiled-in*
# default environment (env.txt-equivalent) uses that - real hardware
# would normally load 192.168.0.1 (or whatever the real device is
# configured with) from its NAND environment partition. Real NAND reads
# are implemented, but the APPSBLENV partition in the available captured
# FULL_FIRMWARE.bin is genuinely erased (all 0xFF), so the compiled-in
# default still applies to that image. A different dump with a valid
# environment may use another guest IP; pass --guest-ip accordingly.
#
# Uses --network host instead of docker's own -p port mapping: with
# -p, connections from the host arrive at QEMU's slirp networking
# with their source address rewritten to docker's bridge gateway
# (172.17.0.1 typically) rather than the configured slirp subnet -
# the guest can't ARP-resolve that address (it's on a different
# subnet entirely) to send its SYN-ACK back, so the TCP handshake
# never completes and every connection just times out. --network host
# sidesteps docker's NAT entirely.

set -euo pipefail

IMAGE=mr80x-qemu:9.1.0
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HTTP_PORT=8080
GUEST_IP=192.168.1.1
RECOVERY=0
NO_NET=0
NAND_IMAGE=""
STOP_AUTOBOOT=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-net)
            NO_NET=1
            shift
            ;;
        --recovery)
            RECOVERY=1
            shift
            ;;
        --http-port)
            HTTP_PORT="$2"
            shift 2
            ;;
        --guest-ip)
            GUEST_IP="$2"
            shift 2
            ;;
        --nand-image)
            NAND_IMAGE="$2"
            shift 2
            ;;
        --stop-autoboot)
            STOP_AUTOBOOT=1
            shift
            ;;
        *)
            KERNEL="$1"
            shift
            ;;
    esac
done

# Auto-detect the real flash dump if --nand-image wasn't given, so
# every real partition (smeminfo, section 4b/17b/20) just shows up
# without needing to remember and type the path every time. Prefer the
# repository-local, gitignored images/ copy; retain the original firmware-lab
# location as a compatibility fallback for existing workspaces.
if [[ -z "$NAND_IMAGE" ]]; then
    LOCAL_NAND_IMAGE="${SCRIPT_DIR}/images/FULL_FIRMWARE.bin"
    FALLBACK_NAND_IMAGE="/media/dados_2tb/opw/openwrt-build-tools/tools/firmware-lab/work/fw_extracted/FULL_FIRMWARE.bin"
    if [[ -f "$LOCAL_NAND_IMAGE" ]]; then
        NAND_IMAGE="$LOCAL_NAND_IMAGE"
        echo "Auto-detected repository-local flash dump: ${NAND_IMAGE}"
    elif [[ -f "$FALLBACK_NAND_IMAGE" ]]; then
        NAND_IMAGE="$FALLBACK_NAND_IMAGE"
        echo "Auto-detected real flash dump: ${NAND_IMAGE} (pass --nand-image PATH to use a different one)"
    fi
fi

if [[ -z "${KERNEL:-}" && -z "$NAND_IMAGE" ]]; then
    echo "usage: $0 [--recovery] [--no-net] [--http-port PORT] [--guest-ip IP] [--nand-image PATH] [--stop-autoboot] [path-to-development-appsbl-elf-or-bin]" >&2
    echo "error: no APPSBL override and no full NAND image was found" >&2
    exit 1
fi

GUEST_SUBNET="$(echo "$GUEST_IP" | sed 's/\.[0-9]*$/.0/')/24"
GUEST_HOST_IP="$(echo "$GUEST_IP" | sed 's/\.[0-9]*$/.2/')"

if [[ "$NO_NET" -eq 1 ]]; then
    NET_ARGS=(-nic none)
else
    NET_ARGS=(-nic "user,model=mr80x-gmac,net=${GUEST_SUBNET},host=${GUEST_HOST_IP},hostfwd=tcp::${HTTP_PORT}-${GUEST_IP}:80")
    echo "HTTP recovery server (once the emulator reaches it) will be at: http://localhost:${HTTP_PORT}/"
fi

DOCKER_ENV=()
if [[ "$RECOVERY" -eq 1 ]]; then
    DOCKER_ENV+=(-e MR80X_RECOVERY=1)
    echo "Recovery mode: modeling the reset button held down, same as real hardware."
fi

if [[ "$STOP_AUTOBOOT" -eq 1 ]]; then
    DOCKER_ENV+=(-e MR80X_STOP_AUTOBOOT=1)
    echo "Autoboot will stop automatically - dropping straight into the u-boot console."
fi

DOCKER_VOLUMES=()
QEMU_BOOT_ARGS=()
if [[ -n "${KERNEL:-}" ]]; then
    KERNEL_DIR="$(cd "$(dirname "$KERNEL")" && pwd)"
    KERNEL_FILE="$(basename "$KERNEL")"
    DOCKER_VOLUMES+=(-v "${KERNEL_DIR}:/fw:ro")
    QEMU_BOOT_ARGS+=(-kernel "/fw/${KERNEL_FILE}")
    echo "Development override: booting APPSBL from ${KERNEL}"
else
    echo "Boot source: APPSBL partition inside ${NAND_IMAGE}"
fi

if [[ -n "$NAND_IMAGE" ]]; then
    NAND_DIR="$(cd "$(dirname "$NAND_IMAGE")" && pwd)"
    NAND_FILE="$(basename "$NAND_IMAGE")"
    DOCKER_VOLUMES+=(-v "${NAND_DIR}:/nand:ro")
    DOCKER_ENV+=(-e "MR80X_NAND_IMAGE=/nand/${NAND_FILE}")
    echo "NAND backed by real flash data: ${NAND_IMAGE}"
fi

echo "Console below IS the TTL/UART connection. Ctrl-A X to quit."
echo

exec docker run --rm -it \
    --network host \
    "${DOCKER_VOLUMES[@]}" \
    "${DOCKER_ENV[@]}" \
    "$IMAGE" \
    /build/qemu-9.1.0/build/qemu-system-arm -M mr80x -nographic -monitor none \
    -serial stdio \
    "${NET_ARGS[@]}" \
    "${QEMU_BOOT_ARGS[@]}"
