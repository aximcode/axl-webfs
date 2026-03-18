#!/bin/bash

# UefiXfer QEMU Launcher
# Boots UefiXfer.efi in QEMU with port forwarding for xfer-server.py

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

QEMU_DIR="$HOME/projects/qemu/install/bin"
QEMU_ARCH="X64"
FORCE_REBUILD=false
XFER_PORT=8080
DISK_SIZE_MB=64
STATE_DIR="$PROJECT_ROOT/build/qemu"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()    { echo -e "${BLUE}[INFO]${NC} $1"; }
log_success() { echo -e "${GREEN}[OK]${NC} $1"; }
log_error()   { echo -e "${RED}[ERROR]${NC} $1" >&2; }

show_usage() {
    echo "Usage: $0 [OPTIONS] {start|stop|status|logs}"
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
    echo "  --port PORT     Host port forwarded to guest 8080 (default: $XFER_PORT)"
}

# Parse options
while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch) QEMU_ARCH="$2"; shift 2 ;;
        --rebuild) FORCE_REBUILD=true; shift ;;
        --port) XFER_PORT="$2"; shift 2 ;;
        --help|-h) show_usage; exit 0 ;;
        *) break ;;
    esac
done

COMMAND="${1:-}"
if [ -z "$COMMAND" ]; then show_usage; exit 1; fi

mkdir -p "$STATE_DIR"

# Architecture config
if [ "$QEMU_ARCH" = "AARCH64" ]; then
    QEMU_BIN="$QEMU_DIR/qemu-system-aarch64"
    FIRMWARE_CODE="/usr/share/AAVMF/AAVMF_CODE.fd"
    FIRMWARE_VARS_ORIG="/usr/share/AAVMF/AAVMF_VARS.fd"
    BOOT_EFI="BOOTAA64.EFI"
    APP_EFI="$PROJECT_ROOT/build/binaries/UefiXfer_AARCH64.efi"
    DRV_EFI="$PROJECT_ROOT/build/binaries/WebDavFsDxe_AARCH64.efi"
else
    QEMU_BIN="$QEMU_DIR/qemu-system-x86_64"
    FIRMWARE_CODE="/usr/share/edk2/ovmf/OVMF_CODE.fd"
    FIRMWARE_VARS_ORIG="/usr/share/edk2/ovmf/OVMF_VARS.fd"
    BOOT_EFI="BOOTX64.EFI"
    APP_EFI="$PROJECT_ROOT/build/binaries/UefiXfer_X64.efi"
    DRV_EFI="$PROJECT_ROOT/build/binaries/WebDavFsDxe_X64.efi"
fi

DISK_IMAGE="$STATE_DIR/uefixfer-${QEMU_ARCH,,}.img"
FIRMWARE_VARS="$STATE_DIR/vars-${QEMU_ARCH,,}.fd"
LOG_FILE="$STATE_DIR/qemu-${QEMU_ARCH,,}.log"
PID_FILE="$STATE_DIR/qemu-${QEMU_ARCH,,}.pid"
MONITOR_SOCK="$STATE_DIR/qemu-${QEMU_ARCH,,}.sock"

create_disk_image() {
    if [ ! -f "$APP_EFI" ]; then
        log_error "UefiXfer.efi not found. Run: scripts/build.sh --arch $QEMU_ARCH"
        exit 1
    fi

    log_info "Creating disk image..."

    dd if=/dev/zero of="$DISK_IMAGE" bs=1M count=$DISK_SIZE_MB status=none
    sgdisk -Z "$DISK_IMAGE" >/dev/null 2>&1
    sgdisk -o "$DISK_IMAGE" >/dev/null 2>&1
    sgdisk -n "1:2048:0" -t 1:EF00 -c 1:"ESP" "$DISK_IMAGE" >/dev/null 2>&1

    local ESP_START ESP_LAST ESP_SECTORS
    ESP_START=$(sgdisk -i 1 "$DISK_IMAGE" 2>/dev/null | awk '/First sector:/{print $3}')
    ESP_LAST=$(sgdisk -i 1 "$DISK_IMAGE" 2>/dev/null | awk '/Last sector:/{print $3}')
    ESP_SECTORS=$((ESP_LAST - ESP_START + 1))

    local ESP_IMG=$(mktemp)
    dd if=/dev/zero of="$ESP_IMG" bs=512 count=$ESP_SECTORS status=none
    mformat -i "$ESP_IMG" -F ::

    # EFI boot directory
    mmd -i "$ESP_IMG" ::EFI
    mmd -i "$ESP_IMG" ::EFI/BOOT

    # Shell.efi as default boot (architecture-specific)
    if [ "$QEMU_ARCH" = "AARCH64" ]; then
        local SHELL_EFI="$HOME/projects/edk2/Build/Shell/DEBUG_GCC5/AARCH64/ShellPkg/Application/Shell/Shell/OUTPUT/Shell.efi"
    else
        local SHELL_EFI="/usr/share/edk2/ovmf/Shell.efi"
    fi
    if [ -f "$SHELL_EFI" ]; then
        mcopy -i "$ESP_IMG" "$SHELL_EFI" "::EFI/BOOT/$BOOT_EFI"
    fi

    # UefiXfer binaries
    mcopy -i "$ESP_IMG" "$APP_EFI" ::UefiXfer.efi
    if [ -f "$DRV_EFI" ]; then
        mcopy -i "$ESP_IMG" "$DRV_EFI" ::WebDavFsDxe.efi
    fi

    # startup.nsh — auto-run on boot
    local NSH=$(mktemp)
    echo "@echo -off" > "$NSH"
    echo "echo UefiXfer QEMU Test Environment" >> "$NSH"
    echo "echo." >> "$NSH"
    echo "echo UefiXfer.efi and WebDavFsDxe.efi are on fs0:" >> "$NSH"
    echo "echo Host xfer-server port forwarded: guest 10.0.2.2:$XFER_PORT" >> "$NSH"
    echo "echo." >> "$NSH"
    mcopy -i "$ESP_IMG" "$NSH" ::startup.nsh
    rm -f "$NSH"

    dd if="$ESP_IMG" of="$DISK_IMAGE" bs=512 seek=$ESP_START conv=notrunc status=none
    rm -f "$ESP_IMG"

    if [ ! -f "$FIRMWARE_VARS" ]; then
        cp "$FIRMWARE_VARS_ORIG" "$FIRMWARE_VARS"
    fi

    log_success "Disk image: $DISK_IMAGE"
}

start_qemu() {
    if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
        log_info "Already running (PID: $(cat "$PID_FILE"))"
        return 0
    fi

    if $FORCE_REBUILD || [ ! -f "$DISK_IMAGE" ] || \
       [ "$APP_EFI" -nt "$DISK_IMAGE" ] || [ "$DRV_EFI" -nt "$DISK_IMAGE" ] 2>/dev/null; then
        create_disk_image
    fi

    log_info "Starting QEMU $QEMU_ARCH (xfer port $XFER_PORT)..."

    local QEMU_ARGS=(
        -name "UefiXfer"
        -m 512M
        -drive "if=pflash,format=raw,readonly=on,file=$FIRMWARE_CODE"
        -drive "if=pflash,format=raw,file=$FIRMWARE_VARS"
        -drive "format=raw,file=$DISK_IMAGE"
        -netdev "user,id=net0,hostfwd=tcp::${XFER_PORT}-:${XFER_PORT}"
        -device "virtio-net-pci,netdev=net0"
        -display none
        -monitor "unix:${MONITOR_SOCK},server,nowait"
        -serial stdio
    )

    if [ "$QEMU_ARCH" = "AARCH64" ]; then
        QEMU_ARGS+=(-machine virt -cpu cortex-a57)
    else
        QEMU_ARGS+=(-machine q35 -enable-kvm -cpu host)
    fi

    "$QEMU_BIN" "${QEMU_ARGS[@]}" > "$LOG_FILE" 2>&1 &
    local PID=$!
    echo "$PID" > "$PID_FILE"
    sleep 2

    if kill -0 "$PID" 2>/dev/null; then
        log_success "QEMU started (PID: $PID)"
        echo ""
        echo "  Serial log:  $LOG_FILE"
        echo "  Monitor:     socat - UNIX-CONNECT:$MONITOR_SOCK"
        echo "  Stop:        $0 --arch $QEMU_ARCH stop"
        echo ""
        echo "  Guest IP for xfer-server: 10.0.2.2:$XFER_PORT"
        echo "  From Shell:  fs0:\\UefiXfer.efi mount http://10.0.2.2:$XFER_PORT/"
    else
        log_error "QEMU failed to start"
        cat "$LOG_FILE"
        rm -f "$PID_FILE"
        exit 1
    fi
}

stop_qemu() {
    if [ -f "$PID_FILE" ]; then
        local PID=$(cat "$PID_FILE")
        if kill -0 "$PID" 2>/dev/null; then
            kill "$PID" 2>/dev/null
            sleep 1
            kill -0 "$PID" 2>/dev/null && kill -9 "$PID" 2>/dev/null
            log_success "QEMU stopped (PID: $PID)"
        else
            log_info "QEMU not running (stale PID)"
        fi
        rm -f "$PID_FILE"
    else
        log_info "QEMU not running"
    fi
    rm -f "$MONITOR_SOCK"
}

case "$COMMAND" in
    start)  start_qemu ;;
    stop)   stop_qemu ;;
    status)
        if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
            log_success "Running (PID: $(cat "$PID_FILE"))"
        else
            log_info "Not running"
        fi
        ;;
    logs)
        if [ -f "$LOG_FILE" ]; then
            tail -50 "$LOG_FILE"
        else
            log_info "No log file"
        fi
        ;;
    *) show_usage; exit 1 ;;
esac
