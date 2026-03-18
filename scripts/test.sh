#!/bin/bash

# UefiXfer Test Suite
# Tests xfer-server.py endpoints and optionally runs QEMU integration tests.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

PASS=0
FAIL=0
SKIP=0

pass() { echo -e "  ${GREEN}PASS${NC} $1"; PASS=$((PASS + 1)); }
fail() { echo -e "  ${RED}FAIL${NC} $1: $2"; FAIL=$((FAIL + 1)); }
skip() { echo -e "  ${YELLOW}SKIP${NC} $1"; SKIP=$((SKIP + 1)); }
info() { echo -e "${BLUE}[$1]${NC} $2"; }

# Parse arguments
RUN_QEMU=false
RUN_AARCH64=false
while [[ $# -gt 0 ]]; do
    case $1 in
        --qemu) RUN_QEMU=true; shift ;;
        --aarch64) RUN_AARCH64=true; RUN_QEMU=true; shift ;;
        --help|-h)
            echo "Usage: $0 [--qemu] [--aarch64]"
            echo "  --qemu      Run QEMU integration tests (X64)"
            echo "  --aarch64   Also test AARCH64 (implies --qemu)"
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ============================================================================
# xfer-server.py tests
# ============================================================================

info "SERVER" "Testing xfer-server.py"

# Create test fixture directory
TEST_DIR=$(mktemp -d)
mkdir -p "$TEST_DIR/subdir"
echo "hello world" > "$TEST_DIR/test.txt"
echo "binary content" > "$TEST_DIR/data.bin"
echo "nested file" > "$TEST_DIR/subdir/nested.txt"
dd if=/dev/urandom of="$TEST_DIR/large.bin" bs=1024 count=100 2>/dev/null

SERVER_PORT=18080
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

# Start server
python3 "$SCRIPT_DIR/xfer-server.py" --root "$TEST_DIR" --port $SERVER_PORT --bind 127.0.0.1 &
SERVER_PID=$!
sleep 0.5

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    fail "server-start" "xfer-server.py failed to start"
    exit 1
fi

BASE="http://127.0.0.1:$SERVER_PORT"

# --- GET /info ---
info "SERVER" "GET /info"
RESP=$(curl -s "$BASE/info")
if echo "$RESP" | python3 -c "import sys,json; d=json.load(sys.stdin); assert d['version']=='1.0'" 2>/dev/null; then
    pass "GET /info returns version 1.0"
else
    fail "GET /info" "unexpected response: $RESP"
fi

# --- GET /list/ (root) ---
info "SERVER" "GET /list/"
RESP=$(curl -s "$BASE/list/")
COUNT=$(echo "$RESP" | python3 -c "import sys,json; print(len(json.load(sys.stdin)))" 2>/dev/null)
if [ "$COUNT" = "4" ]; then
    pass "GET /list/ returns 4 entries"
else
    fail "GET /list/" "expected 4 entries, got $COUNT"
fi

# --- GET /list/ field validation ---
HAS_FIELDS=$(echo "$RESP" | python3 -c "
import sys,json
entries = json.load(sys.stdin)
e = [x for x in entries if x['name'] == 'test.txt'][0]
assert 'name' in e and 'size' in e and 'dir' in e and 'modified' in e
assert e['dir'] == False
assert e['size'] > 0
print('ok')
" 2>/dev/null)
if [ "$HAS_FIELDS" = "ok" ]; then
    pass "GET /list/ entries have name/size/dir/modified fields"
else
    fail "GET /list/ fields" "missing or incorrect fields"
fi

# --- GET /list/subdir/ ---
RESP=$(curl -s "$BASE/list/subdir/")
SUBCOUNT=$(echo "$RESP" | python3 -c "import sys,json; print(len(json.load(sys.stdin)))" 2>/dev/null)
if [ "$SUBCOUNT" = "1" ]; then
    pass "GET /list/subdir/ returns 1 entry"
else
    fail "GET /list/subdir/" "expected 1 entry, got $SUBCOUNT"
fi

# --- GET /list/ nonexistent ---
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/list/nonexistent/")
if [ "$HTTP_CODE" = "404" ]; then
    pass "GET /list/nonexistent/ returns 404"
else
    fail "GET /list/nonexistent/" "expected 404, got $HTTP_CODE"
fi

# --- GET /files/test.txt ---
info "SERVER" "GET /files/"
CONTENT=$(curl -s "$BASE/files/test.txt")
if [ "$CONTENT" = "hello world" ]; then
    pass "GET /files/test.txt returns correct content"
else
    fail "GET /files/test.txt" "unexpected content: $CONTENT"
fi

# --- GET /files/ nonexistent ---
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/files/nonexistent.txt")
if [ "$HTTP_CODE" = "404" ]; then
    pass "GET /files/nonexistent.txt returns 404"
else
    fail "GET /files/nonexistent.txt" "expected 404, got $HTTP_CODE"
fi

# --- GET /files/ nested ---
CONTENT=$(curl -s "$BASE/files/subdir/nested.txt")
if [ "$CONTENT" = "nested file" ]; then
    pass "GET /files/subdir/nested.txt returns correct content"
else
    fail "GET /files/subdir/nested.txt" "unexpected: $CONTENT"
fi

# --- Range request ---
info "SERVER" "Range requests"
PARTIAL=$(curl -s -H "Range: bytes=6-10" "$BASE/files/test.txt")
if [ "$PARTIAL" = "world" ]; then
    pass "Range: bytes=6-10 returns 'world'"
else
    fail "Range request" "expected 'world', got '$PARTIAL'"
fi

RANGE_CODE=$(curl -s -o /dev/null -w "%{http_code}" -H "Range: bytes=6-10" "$BASE/files/test.txt")
if [ "$RANGE_CODE" = "206" ]; then
    pass "Range request returns 206 Partial Content"
else
    fail "Range status" "expected 206, got $RANGE_CODE"
fi

# --- Large file download ---
info "SERVER" "Large file"
curl -s "$BASE/files/large.bin" -o "$TEST_DIR/large_download.bin"
if cmp -s "$TEST_DIR/large.bin" "$TEST_DIR/large_download.bin"; then
    pass "100 KB file downloads correctly"
else
    fail "Large file download" "content mismatch"
fi

# --- PUT /files/ ---
info "SERVER" "PUT /files/"
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -T - "$BASE/files/uploaded.txt" <<< "upload test")
if [ "$HTTP_CODE" = "201" ]; then
    pass "PUT /files/uploaded.txt returns 201"
else
    fail "PUT /files/uploaded.txt" "expected 201, got $HTTP_CODE"
fi

# Verify uploaded content on disk (curl -T <<< adds trailing newline)
DISK_CONTENT=$(cat "$TEST_DIR/uploaded.txt")
if echo "$DISK_CONTENT" | grep -q "upload test"; then
    pass "PUT file written to disk correctly"
else
    fail "PUT file content" "content mismatch on disk: '$DISK_CONTENT'"
fi

# --- PUT creates parent directories ---
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -T - "$BASE/files/newdir/deep/file.txt" <<< "deep file")
if [ "$HTTP_CODE" = "201" ] && [ -f "$TEST_DIR/newdir/deep/file.txt" ]; then
    pass "PUT creates parent directories"
else
    fail "PUT parent dirs" "expected 201 + file on disk"
fi

# --- POST ?mkdir ---
info "SERVER" "POST ?mkdir"
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE/files/newdir2/?mkdir")
if [ "$HTTP_CODE" = "201" ] && [ -d "$TEST_DIR/newdir2" ]; then
    pass "POST ?mkdir creates directory"
else
    fail "POST ?mkdir" "expected 201 + dir on disk, got $HTTP_CODE"
fi

# --- DELETE /files/ ---
info "SERVER" "DELETE /files/"
echo "delete me" > "$TEST_DIR/todelete.txt"
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X DELETE "$BASE/files/todelete.txt")
if [ "$HTTP_CODE" = "200" ] && [ ! -f "$TEST_DIR/todelete.txt" ]; then
    pass "DELETE /files/todelete.txt removes file"
else
    fail "DELETE file" "expected 200 + file gone, got $HTTP_CODE"
fi

# DELETE nonexistent
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X DELETE "$BASE/files/ghost.txt")
if [ "$HTTP_CODE" = "404" ]; then
    pass "DELETE nonexistent returns 404"
else
    fail "DELETE nonexistent" "expected 404, got $HTTP_CODE"
fi

# --- Directory traversal protection ---
info "SERVER" "Security"
# Path traversal: ../../../etc/passwd should not return real /etc/passwd content
TRAVERSAL_RESP=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/files/../../../etc/passwd")
TRAVERSAL_BODY=$(curl -s "$BASE/files/../../../etc/passwd")
if [ "$TRAVERSAL_RESP" = "403" ] || [ "$TRAVERSAL_RESP" = "404" ]; then
    # Verify it didn't actually serve /etc/passwd
    if echo "$TRAVERSAL_BODY" | grep -q "root:"; then
        fail "Path traversal" "served /etc/passwd content!"
    else
        pass "Path traversal blocked ($TRAVERSAL_RESP)"
    fi
else
    fail "Path traversal" "expected 403 or 404, got $TRAVERSAL_RESP"
fi

# --- Read-only mode ---
info "SERVER" "Read-only mode"
kill "$SERVER_PID" 2>/dev/null
wait "$SERVER_PID" 2>/dev/null || true

python3 "$SCRIPT_DIR/xfer-server.py" --root "$TEST_DIR" --port $SERVER_PORT --bind 127.0.0.1 --read-only &
SERVER_PID=$!
sleep 0.5

HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -T - "$BASE/files/blocked.txt" <<< "should fail")
if [ "$HTTP_CODE" = "403" ]; then
    pass "PUT blocked in read-only mode (403)"
else
    fail "Read-only PUT" "expected 403, got $HTTP_CODE"
fi

HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X DELETE "$BASE/files/test.txt")
if [ "$HTTP_CODE" = "403" ]; then
    pass "DELETE blocked in read-only mode (403)"
else
    fail "Read-only DELETE" "expected 403, got $HTTP_CODE"
fi

# GET still works in read-only
CONTENT=$(curl -s "$BASE/files/test.txt")
if [ "$CONTENT" = "hello world" ]; then
    pass "GET works in read-only mode"
else
    fail "Read-only GET" "unexpected: $CONTENT"
fi

# ============================================================================
# QEMU integration tests (optional)
# ============================================================================

if [ "$RUN_QEMU" = true ]; then
    # Run QEMU integration tests for each requested architecture.
    # Default: X64 only. Use --qemu-arch to add AARCH64.
    QEMU_ARCHS=(X64)
    if [ "$RUN_AARCH64" = true ]; then
        QEMU_ARCHS+=(AARCH64)
    fi

    QEMU_DIR="$HOME/projects/qemu/install/bin"
    QEMU_STATE="$PROJECT_ROOT/build/qemu"
    mkdir -p "$QEMU_STATE"

    # Kill server from previous section, start fresh one on port 18080
    kill "$SERVER_PID" 2>/dev/null; wait "$SERVER_PID" 2>/dev/null || true

    # Create fresh test fixture
    QEMU_TEST_DIR=$(mktemp -d)
    echo "mount test content" > "$QEMU_TEST_DIR/readme.txt"
    mkdir -p "$QEMU_TEST_DIR/tools"
    echo "tool data" > "$QEMU_TEST_DIR/tools/sample.txt"

    python3 "$SCRIPT_DIR/xfer-server.py" --root "$QEMU_TEST_DIR" --port $SERVER_PORT --bind 0.0.0.0 &
    SERVER_PID=$!
    sleep 0.5

    for QEMU_ARCH in "${QEMU_ARCHS[@]}"; do
        info "QEMU" "=== Integration tests: $QEMU_ARCH ==="

        if [ "$QEMU_ARCH" = "AARCH64" ]; then
            QEMU_BIN="$QEMU_DIR/qemu-system-aarch64"
            FW_CODE="/usr/share/AAVMF/AAVMF_CODE.fd"
            FW_VARS_ORIG="/usr/share/AAVMF/AAVMF_VARS.fd"
            BOOT_EFI="BOOTAA64.EFI"
            APP_EFI="$PROJECT_ROOT/build/binaries/UefiXfer_AARCH64.efi"
            DRV_EFI="$PROJECT_ROOT/build/binaries/WebDavFsDxe_AARCH64.efi"
            QEMU_MACHINE="-machine virt -cpu cortex-a57"
            BOOT_WAIT=60
        else
            QEMU_BIN="$QEMU_DIR/qemu-system-x86_64"
            FW_CODE="/usr/share/edk2/ovmf/OVMF_CODE.fd"
            FW_VARS_ORIG="/usr/share/edk2/ovmf/OVMF_VARS.fd"
            BOOT_EFI="BOOTX64.EFI"
            APP_EFI="$PROJECT_ROOT/build/binaries/UefiXfer_X64.efi"
            DRV_EFI="$PROJECT_ROOT/build/binaries/WebDavFsDxe_X64.efi"
            QEMU_MACHINE="-machine q35 -enable-kvm -cpu host"
            BOOT_WAIT=20
        fi

        if [ ! -f "$APP_EFI" ] || [ ! -f "$DRV_EFI" ]; then
            skip "$QEMU_ARCH: binaries not built"
            continue
        fi

        if [ ! -f "$QEMU_BIN" ]; then
            skip "$QEMU_ARCH: QEMU binary not found at $QEMU_BIN"
            continue
        fi

        if [ ! -f "$FW_CODE" ]; then
            skip "$QEMU_ARCH: firmware not found at $FW_CODE"
            continue
        fi

        # Build disk image with Shell + UefiXfer + WebDavFsDxe
        DISK="$QEMU_STATE/test-${QEMU_ARCH,,}.img"
        FW_VARS="$QEMU_STATE/vars-test-${QEMU_ARCH,,}.fd"
        SERIAL_LOG="$QEMU_STATE/serial-${QEMU_ARCH,,}.log"
        MON_SOCK="$QEMU_STATE/mon-test-${QEMU_ARCH,,}.sock"

        cp "$FW_VARS_ORIG" "$FW_VARS"

        dd if=/dev/zero of="$DISK" bs=1M count=64 status=none
        sgdisk -Z "$DISK" >/dev/null 2>&1
        sgdisk -o "$DISK" >/dev/null 2>&1
        sgdisk -n "1:2048:0" -t 1:EF00 -c 1:"ESP" "$DISK" >/dev/null 2>&1

        ESP_START=$(sgdisk -i 1 "$DISK" 2>/dev/null | awk '/First sector:/{print $3}')
        ESP_LAST=$(sgdisk -i 1 "$DISK" 2>/dev/null | awk '/Last sector:/{print $3}')
        ESP_SECTORS=$((ESP_LAST - ESP_START + 1))

        ESP_IMG=$(mktemp)
        dd if=/dev/zero of="$ESP_IMG" bs=512 count=$ESP_SECTORS status=none
        mformat -i "$ESP_IMG" -F ::
        mmd -i "$ESP_IMG" ::EFI
        mmd -i "$ESP_IMG" ::EFI/BOOT

        # Shell.efi as boot target (architecture-specific)
        if [ "$QEMU_ARCH" = "AARCH64" ]; then
            SHELL_EFI="$HOME/projects/edk2/Build/Shell/DEBUG_GCC5/AARCH64/ShellPkg/Application/Shell/Shell/OUTPUT/Shell.efi"
        else
            SHELL_EFI="/usr/share/edk2/ovmf/Shell.efi"
        fi
        if [ -f "$SHELL_EFI" ]; then
            mcopy -i "$ESP_IMG" "$SHELL_EFI" "::EFI/BOOT/$BOOT_EFI"
        else
            skip "$QEMU_ARCH: Shell.efi not found at $SHELL_EFI"
            continue
        fi

        mcopy -i "$ESP_IMG" "$APP_EFI" ::UefiXfer.efi
        mcopy -i "$ESP_IMG" "$DRV_EFI" ::WebDavFsDxe.efi

        # startup.nsh: run mount test automatically
        NSH=$(mktemp)
        cat > "$NSH" <<NSHEOF
@echo -off
echo === UefiXfer Integration Test ===
fs0:
UefiXfer.efi mount http://10.0.2.2:${SERVER_PORT}/
map -r
echo === TEST: ls mounted volume ===
ls fs1:\\
echo === TEST: read file ===
type fs1:\\readme.txt
echo === TEST: write file ===
echo "written from uefi" > fs0:\\write_test.txt
cp fs0:\\write_test.txt fs1:\\from_uefi.txt
echo === TEST: read subdir ===
ls fs1:\\tools\\
echo === TESTS COMPLETE ===
reset -s
NSHEOF
        mcopy -i "$ESP_IMG" "$NSH" ::startup.nsh
        rm -f "$NSH"

        dd if="$ESP_IMG" of="$DISK" bs=512 seek=$ESP_START conv=notrunc status=none
        rm -f "$ESP_IMG"

        info "QEMU" "$QEMU_ARCH: Booting (timeout ${BOOT_WAIT}s)..."

        rm -f "$SERIAL_LOG" "$MON_SOCK"

        # Launch QEMU with serial to file, user-mode networking with port forward
        $QEMU_BIN \
            $QEMU_MACHINE \
            -m 512M \
            -drive "if=pflash,format=raw,readonly=on,file=$FW_CODE" \
            -drive "if=pflash,format=raw,file=$FW_VARS" \
            -drive "format=raw,file=$DISK" \
            -netdev "user,id=net0" \
            -device "virtio-net-pci,netdev=net0" \
            -display none \
            -serial "file:$SERIAL_LOG" \
            -monitor "unix:${MON_SOCK},server,nowait" \
            -no-reboot \
            > /dev/null 2>&1 &
        QEMU_PID=$!

        # Wait for QEMU to finish (reset -s in startup.nsh causes shutdown)
        ELAPSED=0
        while kill -0 "$QEMU_PID" 2>/dev/null && [ $ELAPSED -lt $BOOT_WAIT ]; do
            sleep 1
            ELAPSED=$((ELAPSED + 1))
        done

        # Kill if still running
        if kill -0 "$QEMU_PID" 2>/dev/null; then
            kill "$QEMU_PID" 2>/dev/null
            wait "$QEMU_PID" 2>/dev/null || true
        fi

        # Analyze serial output
        if [ ! -f "$SERIAL_LOG" ]; then
            fail "$QEMU_ARCH: serial log" "no output captured"
            continue
        fi

        info "QEMU" "$QEMU_ARCH: Checking results..."

        # Test: Did UefiXfer.efi run?
        if grep -q "UefiXfer" "$SERIAL_LOG" 2>/dev/null; then
            pass "$QEMU_ARCH: UefiXfer.efi executed"
        else
            fail "$QEMU_ARCH: UefiXfer.efi" "not found in serial output"
            echo "--- Serial log ---"
            cat "$SERIAL_LOG"
            echo "--- End ---"
            continue
        fi

        # Test: Did mount succeed? (WebDavFsDxe prints "Mounted successfully")
        if grep -q "Mounted successfully" "$SERIAL_LOG" 2>/dev/null; then
            pass "$QEMU_ARCH: mount connected to xfer-server"
        else
            fail "$QEMU_ARCH: mount" "driver did not report success"
            grep -i "error\|fail\|WebDavFs" "$SERIAL_LOG" 2>/dev/null || true
        fi

        # Test: Did ls show files from xfer-server?
        if grep -q "readme.txt" "$SERIAL_LOG" 2>/dev/null; then
            pass "$QEMU_ARCH: ls shows remote files (readme.txt)"
        else
            fail "$QEMU_ARCH: ls" "readme.txt not found in directory listing"
        fi

        # Test: Did type read the file content?
        if grep -q "mount test content" "$SERIAL_LOG" 2>/dev/null; then
            pass "$QEMU_ARCH: type reads remote file content"
        else
            fail "$QEMU_ARCH: type" "file content not in serial output"
        fi

        # Test: Did write create a file on the workstation?
        if [ -f "$QEMU_TEST_DIR/from_uefi.txt" ]; then
            pass "$QEMU_ARCH: cp wrote file to workstation"
        else
            fail "$QEMU_ARCH: write" "from_uefi.txt not found on workstation"
        fi

        # Test: Did subdir listing work?
        if grep -q "sample.txt" "$SERIAL_LOG" 2>/dev/null; then
            pass "$QEMU_ARCH: ls subdir shows remote files"
        else
            fail "$QEMU_ARCH: ls subdir" "sample.txt not in output"
        fi

        # Test: Did it reach TESTS COMPLETE?
        if grep -q "TESTS COMPLETE" "$SERIAL_LOG" 2>/dev/null; then
            pass "$QEMU_ARCH: all Shell commands completed"
        else
            fail "$QEMU_ARCH: completion" "startup.nsh did not finish"
        fi

        # Clean up for next arch
        rm -f "$QEMU_TEST_DIR/from_uefi.txt"
    done

    rm -rf "$QEMU_TEST_DIR"
fi

# ============================================================================
# Summary
# ============================================================================

echo ""
echo "=========================================="
echo "       UefiXfer Test Summary"
echo "=========================================="
TOTAL=$((PASS + FAIL + SKIP))
echo -e "${GREEN}Passed:${NC}  $PASS"
if [ $FAIL -gt 0 ]; then
    echo -e "${RED}Failed:${NC}  $FAIL"
fi
if [ $SKIP -gt 0 ]; then
    echo -e "${YELLOW}Skipped:${NC} $SKIP"
fi
echo -e "${BLUE}Total:${NC}   $TOTAL"
echo "=========================================="

if [ $FAIL -gt 0 ]; then
    exit 1
fi
