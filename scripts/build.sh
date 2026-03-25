#!/bin/bash

# UefiXfer Build Script
# Builds UefiXfer — UEFI File Transfer Toolkit

set -e

SCRIPT_START_TIME=$(date +%s)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
PARALLEL_JOBS=$(nproc)
BUILD_TIMEOUT=600

# Shared library (logging, EDK2 setup, QEMU/firmware discovery)
DEVKIT_DIR="${DEVKIT_DIR:-$HOME/projects/aximcode/uefi-devkit}"
source "$DEVKIT_DIR/common.sh"

# Parse arguments
CLEAN_BUILD="FALSE"
BUILD_ARCHS=()
BUILD_TARGET="DEBUG"

while [[ $# -gt 0 ]]; do
    case $1 in
        --clean)
            CLEAN_BUILD="TRUE"
            shift
            ;;
        --arch)
            if [[ "$2" != "X64" && "$2" != "AARCH64" ]]; then
                echo "Error: --arch must be X64 or AARCH64"
                exit 1
            fi
            BUILD_ARCHS+=("$2")
            shift 2
            ;;
        --release)
            BUILD_TARGET="RELEASE"
            shift
            ;;
        -n|-j)
            PARALLEL_JOBS="$2"
            shift 2
            ;;
        --help|-h)
            echo "Usage: $0 [--clean] [--arch ARCH]... [--release] [-n JOBS]"
            echo "  --clean      Force clean build"
            echo "  --arch ARCH  Target architecture: X64 or AARCH64 (default: both)"
            echo "               Can be specified multiple times"
            echo "  --release    Build RELEASE instead of DEBUG"
            echo "  -n JOBS      Parallel build jobs (default: $(nproc))"
            exit 0
            ;;
        *)
            echo "Error: Unknown parameter '$1'"
            exit 1
            ;;
    esac
done

# Default to both architectures
if [ ${#BUILD_ARCHS[@]} -eq 0 ]; then
    BUILD_ARCHS=(X64 AARCH64)
fi

# Verify source
log_info "Verifying source code..."
REQUIRED_FILES=(
    "UefiXferPkg/UefiXferPkg.dec"
    "UefiXferPkg/UefiXferPkg.dsc"
    "UefiXferPkg/Application/UefiXfer/UefiXfer.inf"
    "UefiXferPkg/Application/UefiXfer/Main.c"
)

for file in "${REQUIRED_FILES[@]}"; do
    if [ ! -f "$PROJECT_ROOT/$file" ]; then
        log_error "Required file not found: $file"
        exit 1
    fi
done
log_success "Source verified"

# Clean build
if [ "$CLEAN_BUILD" = "TRUE" ]; then
    log_info "Cleaning build directory..."
    rm -rf "$PROJECT_ROOT/Build/UefiXferPkg/"
    log_success "Build directory cleaned"
fi

# Set up build environment
setup_edk2 "$PROJECT_ROOT" || exit 1

# Build for each architecture
mkdir -p "$PROJECT_ROOT/build/binaries"
TOTAL_BUILD_DURATION=0

for BUILD_ARCH in "${BUILD_ARCHS[@]}"; do
    log_info "Building UefiXfer for ${BUILD_ARCH} (${BUILD_TARGET}, ${PARALLEL_JOBS} jobs)..."

    BUILD_START=$(date +%s)
    timeout $BUILD_TIMEOUT build \
        -a ${BUILD_ARCH} \
        -t GCC5 \
        -p UefiXferPkg/UefiXferPkg.dsc \
        -b ${BUILD_TARGET} \
        -n ${PARALLEL_JOBS}
    EXIT_CODE=$?
    BUILD_END=$(date +%s)
    BUILD_DURATION=$((BUILD_END - BUILD_START))
    TOTAL_BUILD_DURATION=$((TOTAL_BUILD_DURATION + BUILD_DURATION))

    if [ $EXIT_CODE -eq 124 ]; then
        log_error "${BUILD_ARCH} build timed out after ${BUILD_TIMEOUT}s!"
        exit 1
    elif [ $EXIT_CODE -ne 0 ]; then
        log_error "${BUILD_ARCH} build failed (exit code $EXIT_CODE)!"
        exit 1
    fi

    APP_PATH="Build/UefiXferPkg/${BUILD_TARGET}_GCC5/${BUILD_ARCH}/UefiXferPkg/Application/UefiXfer/UefiXfer/OUTPUT/UefiXfer.efi"
    if [ ! -f "$APP_PATH" ]; then
        log_error "${BUILD_ARCH}: UefiXfer.efi not found!"
        exit 1
    fi

    BINARY_SIZE=$(stat -c%s "$APP_PATH")
    log_success "${BUILD_ARCH}: UefiXfer.efi (${BINARY_SIZE} bytes, ${BUILD_DURATION}s)"

    cp "$APP_PATH" "$PROJECT_ROOT/build/binaries/UefiXfer_${BUILD_ARCH}.efi"

    # Copy WebDavFsDxe driver
    DRV_PATH="Build/UefiXferPkg/${BUILD_TARGET}_GCC5/${BUILD_ARCH}/UefiXferPkg/Driver/WebDavFsDxe/WebDavFsDxe/OUTPUT/WebDavFsDxe.efi"
    if [ -f "$DRV_PATH" ]; then
        DRV_SIZE=$(stat -c%s "$DRV_PATH")
        log_success "${BUILD_ARCH}: WebDavFsDxe.efi (${DRV_SIZE} bytes)"
        cp "$DRV_PATH" "$PROJECT_ROOT/build/binaries/WebDavFsDxe_${BUILD_ARCH}.efi"
    fi
done

# Summary
SCRIPT_END=$(date +%s)
TOTAL=$((SCRIPT_END - SCRIPT_START_TIME))
MINS=$((TOTAL / 60))
SECS=$((TOTAL % 60))

echo ""
echo "=========================================="
echo "       UefiXfer Build Summary"
echo "=========================================="
for BUILD_ARCH in "${BUILD_ARCHS[@]}"; do
    APP_PATH="Build/UefiXferPkg/${BUILD_TARGET}_GCC5/${BUILD_ARCH}/UefiXferPkg/Application/UefiXfer/UefiXfer/OUTPUT/UefiXfer.efi"
    BINARY_SIZE=$(stat -c%s "$APP_PATH")
    echo -e "${BLUE}${BUILD_ARCH}:${NC}  UefiXfer.efi (${BINARY_SIZE} bytes) -> build/binaries/UefiXfer_${BUILD_ARCH}.efi"
done
if [ $MINS -gt 0 ]; then
    echo -e "${BLUE}Build Time:${NC} ${MINS}m ${SECS}s (EDK2: ${TOTAL_BUILD_DURATION}s)"
else
    echo -e "${BLUE}Build Time:${NC} ${SECS}s (EDK2: ${TOTAL_BUILD_DURATION}s)"
fi
echo "=========================================="
echo ""

log_success "Build completed!"
