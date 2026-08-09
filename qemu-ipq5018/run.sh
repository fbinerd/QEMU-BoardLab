#!/bin/bash
# Boots a real appsbl ELF in the mr80x QEMU machine, with interactive
# UART console (same as connecting a TTL adapter to the real router)
# and, unless --no-net is given, network access (same as plugging an
# Ethernet cable into the router's LAN port) with the recovery HTTP
# server reachable from the host.
#
# Usage:
#   ./run.sh [--no-net] [--http-port PORT] <path-to-appsbl.unpadded.elf-or-.bin>
#
# Examples:
#   ./run.sh /media/dados_2tb/appsbl/out/appsbl.unpadded.elf
#   ./run.sh /media/dados_2tb/appsbl/out/appsbl-dual-key.bin
#   ./run.sh --http-port 9090 /media/dados_2tb/appsbl/out/appsbl.unpadded.elf
#
# Console: this terminal IS the TTL/UART connection - type at the
# prompt exactly like you would over a real serial adapter. Ctrl-A X
# quits QEMU.
#
# Network: with networking on (the default), the recovery HTTP server
# (192.168.0.1:80 inside the emulator) is reachable at
# http://localhost:<http-port>/ on the host (default port 8080).

set -euo pipefail

IMAGE=mr80x-qemu:9.1.0
HTTP_PORT=8080
NET_ARGS=(-nic "user,model=mr80x-gmac,net=192.168.0.0/24,host=192.168.0.2,hostfwd=tcp::${HTTP_PORT}-192.168.0.1:80")

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-net)
            NET_ARGS=(-nic none)
            shift
            ;;
        --http-port)
            HTTP_PORT="$2"
            NET_ARGS=(-nic "user,model=mr80x-gmac,net=192.168.0.0/24,host=192.168.0.2,hostfwd=tcp::${HTTP_PORT}-192.168.0.1:80")
            shift 2
            ;;
        *)
            KERNEL="$1"
            shift
            ;;
    esac
done

if [[ -z "${KERNEL:-}" ]]; then
    echo "usage: $0 [--no-net] [--http-port PORT] <path-to-appsbl-elf-or-bin>" >&2
    exit 1
fi

KERNEL_DIR="$(cd "$(dirname "$KERNEL")" && pwd)"
KERNEL_FILE="$(basename "$KERNEL")"

if [[ "${NET_ARGS[1]}" != "none" ]]; then
    echo "HTTP recovery server (once the emulator reaches it) will be at: http://localhost:${HTTP_PORT}/"
fi
echo "Console below IS the TTL/UART connection. Ctrl-A X to quit."
echo

exec docker run --rm -it \
    -v "${KERNEL_DIR}:/fw:ro" \
    -p "${HTTP_PORT}:${HTTP_PORT}" \
    "$IMAGE" \
    /build/qemu-9.1.0/build/qemu-system-arm -M mr80x -nographic -monitor none \
    -serial stdio \
    "${NET_ARGS[@]}" \
    -kernel "/fw/${KERNEL_FILE}"
