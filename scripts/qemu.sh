#!/bin/bash

# UefiXfer QEMU Launcher
# Boots UefiXfer.efi in QEMU with port forwarding for xfer-server.py
# Thin wrapper around uefi-devkit common.sh qemu_launch

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Shared library
DEVKIT_DIR="${DEVKIT_DIR:-$HOME/projects/aximcode/uefi-devkit}"
source "$DEVKIT_DIR/common.sh"

# Defaults
QEMU_ARCH="X64"
FORCE_REBUILD=false
XFER_PORT=8080

show_usage() {
    echo "Usage: $0 [OPTIONS] {start|stop|status|logs} [-- QEMU_ARGS...]"
    echo ""
    echo "Commands:"
    echo "  start     Boot UEFI Shell with UefiXfer + WebDavFsDxe on ESP"
    echo "  stop      Stop QEMU"
    echo "  status    Check if running"
    echo "  logs      Show serial output"
    echo ""
    echo "Options:"
    echo "  --arch ARCH     X64 (default) or AARCH64"
    echo "  --rebuild       Force disk image recreation"
    echo "  --port PORT     Host port forwarded to guest (default: $XFER_PORT)"
    echo ""
    echo "Pass-through QEMU arguments after --:"
    echo "  $0 start -- -m 1G -smp 4"
}

# Parse options
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) QEMU_ARCH="$2"; shift 2 ;;
        --rebuild) FORCE_REBUILD=true; shift ;;
        --port) XFER_PORT="$2"; shift 2 ;;
        --help|-h) show_usage; exit 0 ;;
        --) shift; QEMU_PASSTHROUGH=("$@"); shift $#; break ;;
        *) break ;;
    esac
done

COMMAND="${1:-}"
shift || true
if [[ "${1:-}" == "--" ]]; then shift; QEMU_PASSTHROUGH=("$@"); fi
if [[ -z "$COMMAND" ]]; then show_usage; exit 1; fi

# Project-specific configuration
QEMU_NAME="UefiXfer"
QEMU_STATE_DIR="$PROJECT_ROOT/build/qemu"
QEMU_APPS=(
    "$PROJECT_ROOT/build/binaries/UefiXfer_${QEMU_ARCH}.efi:UefiXfer.efi"
    "$PROJECT_ROOT/build/binaries/WebDavFsDxe_${QEMU_ARCH}.efi:WebDavFsDxe.efi"
)
QEMU_PORTS=("${XFER_PORT}:${XFER_PORT}")

QEMU_STARTUP_NSH="$(cat <<NSH
@echo -off
echo UefiXfer QEMU Test Environment
echo
echo UefiXfer.efi and WebDavFsDxe.efi are on the ESP
echo Host xfer-server port forwarded: guest 10.0.2.2:${XFER_PORT}
echo
NSH
)"

# Pass-through QEMU arguments
QEMU_EXTRA_ARGS=()
if [[ -n "${QEMU_PASSTHROUGH+x}" && ${#QEMU_PASSTHROUGH[@]} -gt 0 ]]; then
    QEMU_EXTRA_ARGS=("${QEMU_PASSTHROUGH[@]}")
fi

# Rebuild flag
REBUILD_ARGS=()
if $FORCE_REBUILD; then
    REBUILD_ARGS=(--rebuild)
fi

qemu_launch "$COMMAND" "${REBUILD_ARGS[@]}"
