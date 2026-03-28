#!/bin/bash

# HttpFS Test Suite
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

# --- Rename (POST ?rename) ---
info "SERVER" "Rename"
echo "rename me" > "$TEST_DIR/before.txt"
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE/files/before.txt?rename=after.txt")
if [ "$HTTP_CODE" = "200" ] && [ -f "$TEST_DIR/after.txt" ] && [ ! -f "$TEST_DIR/before.txt" ]; then
    pass "POST ?rename renames file"
else
    fail "POST ?rename" "expected 200 + file moved, got $HTTP_CODE"
fi

# Verify content preserved
if [ -f "$TEST_DIR/after.txt" ] && [ "$(cat "$TEST_DIR/after.txt")" = "rename me" ]; then
    pass "Renamed file content preserved"
else
    fail "Rename content" "content mismatch after rename"
fi
rm -f "$TEST_DIR/after.txt"

# Rename nonexistent
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE/files/ghost.txt?rename=other.txt")
if [ "$HTTP_CODE" = "404" ]; then
    pass "Rename nonexistent returns 404"
else
    fail "Rename nonexistent" "expected 404, got $HTTP_CODE"
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
    # Large file for transfer test (~50KB of repeating text)
    python3 -c "print('large file line\\n' * 3000, end='')" > "$QEMU_TEST_DIR/large.txt"

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
            APP_EFI="$PROJECT_ROOT/build/binaries/HttpFS_AARCH64.efi"
            DRV_EFI="$PROJECT_ROOT/build/binaries/WebDavFsDxe_AARCH64.efi"
            QEMU_MACHINE="-machine virt -cpu cortex-a57"
            BOOT_WAIT=60
        else
            QEMU_BIN="$QEMU_DIR/qemu-system-x86_64"
            FW_CODE="/usr/share/edk2/ovmf/OVMF_CODE.fd"
            FW_VARS_ORIG="/usr/share/edk2/ovmf/OVMF_VARS.fd"
            BOOT_EFI="BOOTX64.EFI"
            APP_EFI="$PROJECT_ROOT/build/binaries/HttpFS_X64.efi"
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

        # Copy arch-specific TestApp for exec-from-mount test
        cp "$APP_EFI" "$QEMU_TEST_DIR/TestApp.efi"

        # Build disk image with Shell + HttpFS + WebDavFsDxe
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

        mcopy -i "$ESP_IMG" "$APP_EFI" ::HttpFS.efi
        mcopy -i "$ESP_IMG" "$DRV_EFI" ::WebDavFsDxe.efi

        # startup.nsh: run mount test automatically
        NSH=$(mktemp)
        cat > "$NSH" <<NSHEOF
@echo -off
echo === HttpFS Integration Test ===
fs0:
HttpFS.efi mount http://10.0.2.2:${SERVER_PORT}/
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
echo === TEST: exec from mount ===
fs1:\\TestApp.efi -h
echo === TEST: large file read ===
type fs1:\\large.txt
echo === TEST: umount ===
HttpFS.efi umount
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

        # Test: Did HttpFS.efi run?
        if grep -q "HttpFS" "$SERIAL_LOG" 2>/dev/null; then
            pass "$QEMU_ARCH: HttpFS.efi executed"
        else
            fail "$QEMU_ARCH: HttpFS.efi" "not found in serial output"
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

        # Test: Did exec from mounted volume work? (TestApp.efi = HttpFS, prints version)
        if grep -q "HttpFS v0.1" "$SERIAL_LOG" 2>/dev/null; then
            pass "$QEMU_ARCH: exec .efi from mounted volume"
        else
            fail "$QEMU_ARCH: exec from mount" "HttpFS v0.1 not in output"
        fi

        # Test: Did large file read work?
        if grep -q "large file line" "$SERIAL_LOG" 2>/dev/null; then
            pass "$QEMU_ARCH: large file read (50KB)"
        else
            fail "$QEMU_ARCH: large file" "content not in output"
        fi

        # Test: Did umount work?
        if grep -q "Unmounted" "$SERIAL_LOG" 2>/dev/null; then
            pass "$QEMU_ARCH: umount succeeded"
        else
            fail "$QEMU_ARCH: umount" "Unmounted not in output"
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

    # ========================================================================
    # Serve integration tests (QEMU runs HttpFS serve, host runs curl)
    # ========================================================================

    SERVE_PORT=18090

    for QEMU_ARCH in "${QEMU_ARCHS[@]}"; do
        info "QEMU" "=== Serve integration tests: $QEMU_ARCH ==="

        if [ "$QEMU_ARCH" = "AARCH64" ]; then
            QEMU_BIN="$QEMU_DIR/qemu-system-aarch64"
            FW_CODE="/usr/share/AAVMF/AAVMF_CODE.fd"
            FW_VARS_ORIG="/usr/share/AAVMF/AAVMF_VARS.fd"
            BOOT_EFI="BOOTAA64.EFI"
            APP_EFI="$PROJECT_ROOT/build/binaries/HttpFS_AARCH64.efi"
            DRV_EFI="$PROJECT_ROOT/build/binaries/WebDavFsDxe_AARCH64.efi"
            QEMU_MACHINE="-machine virt -cpu cortex-a57"
            BOOT_WAIT=45
        else
            QEMU_BIN="$QEMU_DIR/qemu-system-x86_64"
            FW_CODE="/usr/share/edk2/ovmf/OVMF_CODE.fd"
            FW_VARS_ORIG="/usr/share/edk2/ovmf/OVMF_VARS.fd"
            BOOT_EFI="BOOTX64.EFI"
            APP_EFI="$PROJECT_ROOT/build/binaries/HttpFS_X64.efi"
            DRV_EFI="$PROJECT_ROOT/build/binaries/WebDavFsDxe_X64.efi"
            QEMU_MACHINE="-machine q35 -enable-kvm -cpu host"
            BOOT_WAIT=30
        fi

        if [ ! -f "$APP_EFI" ] || [ ! -f "$QEMU_BIN" ] || [ ! -f "$FW_CODE" ]; then
            skip "$QEMU_ARCH serve: missing prerequisites"
            continue
        fi

        DISK="$QEMU_STATE/serve-${QEMU_ARCH,,}.img"
        FW_VARS="$QEMU_STATE/vars-serve-${QEMU_ARCH,,}.fd"
        SERIAL_LOG="$QEMU_STATE/serial-serve-${QEMU_ARCH,,}.log"
        MON_SOCK="$QEMU_STATE/mon-serve-${QEMU_ARCH,,}.sock"

        cp "$FW_VARS_ORIG" "$FW_VARS"

        # Build disk with startup.nsh that runs serve
        dd if=/dev/zero of="$DISK" bs=1M count=64 status=none
        sgdisk -Z "$DISK" >/dev/null 2>&1
        sgdisk -o "$DISK" >/dev/null 2>&1
        sgdisk -n "1:2048:0" -t 1:EF00 "$DISK" >/dev/null 2>&1

        ESP_START=$(sgdisk -i 1 "$DISK" 2>/dev/null | awk '/First sector:/{print $3}')
        ESP_LAST=$(sgdisk -i 1 "$DISK" 2>/dev/null | awk '/Last sector:/{print $3}')
        ESP_SECTORS=$((ESP_LAST - ESP_START + 1))

        ESP_IMG=$(mktemp)
        dd if=/dev/zero of="$ESP_IMG" bs=512 count=$ESP_SECTORS status=none
        mformat -i "$ESP_IMG" -F ::
        mmd -i "$ESP_IMG" ::EFI
        mmd -i "$ESP_IMG" ::EFI/BOOT

        if [ "$QEMU_ARCH" = "AARCH64" ]; then
            SHELL_EFI="$HOME/projects/edk2/Build/Shell/DEBUG_GCC5/AARCH64/ShellPkg/Application/Shell/Shell/OUTPUT/Shell.efi"
        else
            SHELL_EFI="/usr/share/edk2/ovmf/Shell.efi"
        fi
        if [ ! -f "$SHELL_EFI" ]; then
            skip "$QEMU_ARCH serve: Shell.efi not found"
            rm -f "$ESP_IMG"
            continue
        fi
        mcopy -i "$ESP_IMG" "$SHELL_EFI" "::EFI/BOOT/$BOOT_EFI"
        mcopy -i "$ESP_IMG" "$APP_EFI" ::HttpFS.efi

        # Also put a test file on the ESP for download testing
        echo "serve test file" > /tmp/serve_test.txt
        mcopy -i "$ESP_IMG" /tmp/serve_test.txt ::serve_test.txt
        rm -f /tmp/serve_test.txt

        # startup.nsh: run serve (blocks until ESC)
        NSH=$(mktemp)
        echo "@echo -off" > "$NSH"
        echo "fs0:" >> "$NSH"
        echo "HttpFS.efi serve -p 8080" >> "$NSH"
        mcopy -i "$ESP_IMG" "$NSH" ::startup.nsh
        rm -f "$NSH"

        dd if="$ESP_IMG" of="$DISK" bs=512 seek=$ESP_START conv=notrunc status=none
        rm -f "$ESP_IMG"

        info "QEMU" "$QEMU_ARCH serve: Booting (wait ${BOOT_WAIT}s for server start)..."

        rm -f "$SERIAL_LOG" "$MON_SOCK"

        # Port forward: host SERVE_PORT -> guest 8080
        $QEMU_BIN \
            $QEMU_MACHINE \
            -m 512M \
            -drive "if=pflash,format=raw,readonly=on,file=$FW_CODE" \
            -drive "if=pflash,format=raw,file=$FW_VARS" \
            -drive "format=raw,file=$DISK" \
            -netdev "user,id=net0,hostfwd=tcp::${SERVE_PORT}-:8080" \
            -device "virtio-net-pci,netdev=net0" \
            -display none \
            -serial "file:$SERIAL_LOG" \
            -monitor "unix:${MON_SOCK},server,nowait" \
            > /dev/null 2>&1 &
        QEMU_PID=$!

        # Wait for server to be ready (poll curl until it responds)
        READY=false
        for WAIT in $(seq 1 $BOOT_WAIT); do
            sleep 1
            if ! kill -0 "$QEMU_PID" 2>/dev/null; then break; fi
            HTTP_CHECK=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:${SERVE_PORT}/" 2>/dev/null || true)
            if [ "$HTTP_CHECK" = "200" ]; then
                READY=true
                break
            fi
        done

        if ! $READY; then
            fail "$QEMU_ARCH serve: server did not start within ${BOOT_WAIT}s"
            kill "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null || true
            if [ -f "$SERIAL_LOG" ]; then
                cat "$SERIAL_LOG" | strings | grep -i "error\|fail\|HttpFS\|Listening" | head -10
            fi
            continue
        fi

        BASE="http://127.0.0.1:${SERVE_PORT}"

        # Test: GET / (volume list)
        RESP=$(curl -sf -H "Accept: application/json" "$BASE/" 2>/dev/null)
        if echo "$RESP" | grep -q "fs0"; then
            pass "$QEMU_ARCH serve: GET / returns volume list with fs0"
        else
            fail "$QEMU_ARCH serve: GET /" "no fs0 in response: $RESP"
        fi

        # Test: GET /fs0/ (directory listing)
        RESP=$(curl -sf -H "Accept: application/json" "$BASE/fs0/" 2>/dev/null)
        if echo "$RESP" | grep -q "HttpFS.efi"; then
            pass "$QEMU_ARCH serve: GET /fs0/ lists files"
        else
            fail "$QEMU_ARCH serve: GET /fs0/" "HttpFS.efi not in listing"
        fi

        # Test: GET file download
        CONTENT=$(curl -sf "$BASE/fs0/serve_test.txt" 2>/dev/null)
        if echo "$CONTENT" | grep -q "serve test file"; then
            pass "$QEMU_ARCH serve: GET /fs0/serve_test.txt downloads file"
        else
            fail "$QEMU_ARCH serve: file download" "unexpected: $CONTENT"
        fi

        # Test: PUT file upload
        HTTP_CODE=$(echo -n "upload data!" | curl -sf -o /dev/null -w "%{http_code}" \
            -T - "$BASE/fs0/uploaded.txt" 2>/dev/null)
        if [ "$HTTP_CODE" = "201" ]; then
            pass "$QEMU_ARCH serve: PUT /fs0/uploaded.txt returns 201"
        else
            fail "$QEMU_ARCH serve: PUT" "expected 201, got $HTTP_CODE"
        fi

        # Test: Read back uploaded file
        CONTENT=$(curl -sf "$BASE/fs0/uploaded.txt" 2>/dev/null)
        if echo "$CONTENT" | grep -q "upload data"; then
            pass "$QEMU_ARCH serve: uploaded file readable"
        else
            fail "$QEMU_ARCH serve: read upload" "content: $CONTENT"
        fi

        # Test: DELETE file
        HTTP_CODE=$(curl -sf -o /dev/null -w "%{http_code}" -X DELETE "$BASE/fs0/uploaded.txt" 2>/dev/null)
        if [ "$HTTP_CODE" = "200" ]; then
            pass "$QEMU_ARCH serve: DELETE returns 200"
        else
            fail "$QEMU_ARCH serve: DELETE" "expected 200, got $HTTP_CODE"
        fi

        # Test: POST mkdir
        HTTP_CODE=$(curl -sf -o /dev/null -w "%{http_code}" -X POST "$BASE/fs0/testdir/?mkdir" 2>/dev/null)
        if [ "$HTTP_CODE" = "201" ]; then
            pass "$QEMU_ARCH serve: POST ?mkdir returns 201"
        else
            fail "$QEMU_ARCH serve: mkdir" "expected 201, got $HTTP_CODE"
        fi

        # Test: Range request (partial download)
        # serve_test.txt contains "serve test file\n" (16 bytes)
        PARTIAL=$(curl -sf -H "Range: bytes=6-9" "$BASE/fs0/serve_test.txt" 2>/dev/null)
        RANGE_CODE=$(curl -sf -o /dev/null -w "%{http_code}" -H "Range: bytes=6-9" "$BASE/fs0/serve_test.txt" 2>/dev/null)
        if [ "$RANGE_CODE" = "206" ] && [ "$PARTIAL" = "test" ]; then
            pass "$QEMU_ARCH serve: Range request returns 206 + correct bytes"
        else
            fail "$QEMU_ARCH serve: Range" "code=$RANGE_CODE content='$PARTIAL'"
        fi

        # Test: 404 on bad volume
        HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/fs99/anything" 2>/dev/null || true)
        if [ "$HTTP_CODE" = "404" ]; then
            pass "$QEMU_ARCH serve: bad volume returns 404"
        else
            fail "$QEMU_ARCH serve: bad volume" "expected 404, got $HTTP_CODE"
        fi

        # Test: 404 on nonexistent file
        HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/fs0/nonexistent.xyz" 2>/dev/null || true)
        if [ "$HTTP_CODE" = "404" ]; then
            pass "$QEMU_ARCH serve: nonexistent file returns 404"
        else
            fail "$QEMU_ARCH serve: nonexistent" "expected 404, got $HTTP_CODE"
        fi

        # Kill QEMU — quit via monitor, then force kill
        echo "quit" | socat - "UNIX-CONNECT:$MON_SOCK" > /dev/null 2>&1 || true
        sleep 1
        kill "$QEMU_PID" 2>/dev/null || true; sleep 1
        kill -9 "$QEMU_PID" 2>/dev/null || true; wait "$QEMU_PID" 2>/dev/null || true
    done

    # ====================================================================
    # Serve read-only mode test (X64 only — same code path for AARCH64)
    # ====================================================================

    info "QEMU" "=== Serve read-only mode test: X64 ==="

    QEMU_BIN="$QEMU_DIR/qemu-system-x86_64"
    FW_CODE="/usr/share/edk2/ovmf/OVMF_CODE.fd"
    FW_VARS_ORIG="/usr/share/edk2/ovmf/OVMF_VARS.fd"
    APP_EFI="$PROJECT_ROOT/build/binaries/HttpFS_X64.efi"

    DISK="$QEMU_STATE/serve-ro.img"
    FW_VARS="$QEMU_STATE/vars-serve-ro.fd"
    SERIAL_LOG="$QEMU_STATE/serial-serve-ro.log"
    MON_SOCK="$QEMU_STATE/mon-serve-ro.sock"

    cp "$FW_VARS_ORIG" "$FW_VARS"

    dd if=/dev/zero of="$DISK" bs=1M count=64 status=none
    sgdisk -Z "$DISK" >/dev/null 2>&1
    sgdisk -o "$DISK" >/dev/null 2>&1
    sgdisk -n "1:2048:0" -t 1:EF00 "$DISK" >/dev/null 2>&1
    ESP_START=$(sgdisk -i 1 "$DISK" 2>/dev/null | awk '/First sector:/{print $3}')
    ESP_LAST=$(sgdisk -i 1 "$DISK" 2>/dev/null | awk '/Last sector:/{print $3}')
    ESP_SECTORS=$((ESP_LAST - ESP_START + 1))

    ESP_IMG=$(mktemp)
    dd if=/dev/zero of="$ESP_IMG" bs=512 count=$ESP_SECTORS status=none
    mformat -i "$ESP_IMG" -F ::
    mmd -i "$ESP_IMG" ::EFI ::EFI/BOOT
    mcopy -i "$ESP_IMG" /usr/share/edk2/ovmf/Shell.efi "::EFI/BOOT/BOOTX64.EFI"
    mcopy -i "$ESP_IMG" "$APP_EFI" ::HttpFS.efi
    echo "readonly test" > /tmp/ro_test.txt
    mcopy -i "$ESP_IMG" /tmp/ro_test.txt ::ro_test.txt
    rm -f /tmp/ro_test.txt

    NSH=$(mktemp)
    echo "@echo -off" > "$NSH"
    echo "fs0:" >> "$NSH"
    echo "HttpFS.efi serve -p 8080 --read-only" >> "$NSH"
    mcopy -i "$ESP_IMG" "$NSH" ::startup.nsh
    rm -f "$NSH"

    dd if="$ESP_IMG" of="$DISK" bs=512 seek=$ESP_START conv=notrunc status=none
    rm -f "$ESP_IMG"

    rm -f "$SERIAL_LOG" "$MON_SOCK"
    $QEMU_BIN -enable-kvm -machine q35 -cpu host -m 512M \
        -drive "if=pflash,format=raw,readonly=on,file=$FW_CODE" \
        -drive "if=pflash,format=raw,file=$FW_VARS" \
        -drive "format=raw,file=$DISK" \
        -netdev "user,id=net0,hostfwd=tcp::${SERVE_PORT}-:8080" \
        -device "virtio-net-pci,netdev=net0" \
        -display none -serial "file:$SERIAL_LOG" \
        -monitor "unix:${MON_SOCK},server,nowait" \
        > /dev/null 2>&1 &
    QEMU_PID=$!

    READY=false
    for WAIT in $(seq 1 30); do
        sleep 1
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then break; fi
        HTTP_CHECK=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:${SERVE_PORT}/" 2>/dev/null || true)
        if [ "$HTTP_CHECK" = "200" ]; then
            READY=true; break
        fi
    done

    BASE="http://127.0.0.1:${SERVE_PORT}"

    if $READY; then
        # GET should work
        CONTENT=$(curl -sf "$BASE/fs0/ro_test.txt" 2>/dev/null)
        if echo "$CONTENT" | grep -q "readonly test"; then
            pass "serve --read-only: GET works"
        else
            fail "serve --read-only: GET" "unexpected: $CONTENT"
        fi

        # PUT should be blocked
        HTTP_CODE=$(echo -n "blocked" | curl -s -o /dev/null -w "%{http_code}" -T - "$BASE/fs0/blocked.txt" 2>/dev/null || true)
        if [ "$HTTP_CODE" = "403" ]; then
            pass "serve --read-only: PUT blocked (403)"
        else
            fail "serve --read-only: PUT" "expected 403, got $HTTP_CODE"
        fi

        # DELETE should be blocked
        HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X DELETE "$BASE/fs0/ro_test.txt" 2>/dev/null || true)
        if [ "$HTTP_CODE" = "403" ]; then
            pass "serve --read-only: DELETE blocked (403)"
        else
            fail "serve --read-only: DELETE" "expected 403, got $HTTP_CODE"
        fi
    else
        fail "serve --read-only: server start" "did not start within 15s"
    fi

    echo "quit" | socat - "UNIX-CONNECT:$MON_SOCK" > /dev/null 2>&1 || true
    sleep 1
    kill "$QEMU_PID" 2>/dev/null || true; sleep 1
    kill -9 "$QEMU_PID" 2>/dev/null || true; wait "$QEMU_PID" 2>/dev/null || true
fi

# ============================================================================
# Summary
# ============================================================================

echo ""
echo "=========================================="
echo "       HttpFS Test Summary"
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
