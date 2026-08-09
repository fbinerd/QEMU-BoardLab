#!/bin/bash
# Boots a real appsbl ELF in the mr80x QEMU machine, with interactive
# UART console (same as connecting a TTL adapter to the real router)
# and, unless --no-net is given, network access (same as plugging an
# Ethernet cable into the router's LAN port) with the recovery HTTP
# server reachable from the host.
#
# Usage:
#   ./run.sh [--recovery] [--no-net] [--http-port PORT] [--guest-ip IP] <path-to-appsbl.unpadded.elf-or-.bin>
#
# Examples:
#   ./run.sh /media/dados_2tb/appsbl/out/appsbl.unpadded.elf
#   ./run.sh --recovery /media/dados_2tb/appsbl/out/appsbl.unpadded.elf
#   ./run.sh --http-port 9090 /media/dados_2tb/appsbl/out/appsbl.unpadded.elf
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
# configured with) from its NAND environment partition, but this
# emulator doesn't yet implement real NAND *data* reads (only device
# ID detection), so "*** Warning - readenv() failed, using default
# environment" always fires and the compiled-in default applies. If a
# future version of this emulator backs NAND with real firmware data
# (see BRINGUP-NOTES.md "next steps"), the effective IP may go back to
# matching the real device and this default should be revisited.
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
HTTP_PORT=8080
GUEST_IP=192.168.1.1
RECOVERY=0
NO_NET=0

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
        *)
            KERNEL="$1"
            shift
            ;;
    esac
done

if [[ -z "${KERNEL:-}" ]]; then
    echo "usage: $0 [--recovery] [--no-net] [--http-port PORT] [--guest-ip IP] <path-to-appsbl-elf-or-bin>" >&2
    exit 1
fi

KERNEL_DIR="$(cd "$(dirname "$KERNEL")" && pwd)"
KERNEL_FILE="$(basename "$KERNEL")"

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
    DOCKER_ENV=(-e MR80X_RECOVERY=1)
    echo "Recovery mode: modeling the reset button held down, same as real hardware."
fi

echo "Console below IS the TTL/UART connection. Ctrl-A X to quit."
echo

exec docker run --rm -it \
    --network host \
    -v "${KERNEL_DIR}:/fw:ro" \
    "${DOCKER_ENV[@]}" \
    "$IMAGE" \
    /build/qemu-9.1.0/build/qemu-system-arm -M mr80x -nographic -monitor none \
    -serial stdio \
    "${NET_ARGS[@]}" \
    -kernel "/fw/${KERNEL_FILE}"
