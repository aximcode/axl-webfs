#!/bin/bash
# Copyright 2026 AximCode
# SPDX-License-Identifier: Apache-2.0
#
# axl-webfs Test Suite
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
# Uses AXL SDK's run-qemu.sh for QEMU/firmware/disk-image management.
# run-qemu.sh ships with the axl-sdk source tree (not the .deb/.rpm).
# Point AXL_SDK_SRC at an axl-sdk checkout to run QEMU tests.
# ============================================================================

if [ "$RUN_QEMU" = true ]; then
    RUN_QEMU_SH="${AXL_SDK_SRC:+$AXL_SDK_SRC/scripts/run-qemu.sh}"

    if [ -z "$RUN_QEMU_SH" ] || [ ! -f "$RUN_QEMU_SH" ]; then
        skip "QEMU tests: set AXL_SDK_SRC to an axl-sdk source checkout (run-qemu.sh not packaged in .deb/.rpm)"
    else

    QEMU_ARCHS=(X64)
    if [ "$RUN_AARCH64" = true ]; then
        QEMU_ARCHS+=(AARCH64)
    fi

    # Kill server from previous section, start fresh one bound to all interfaces
    kill "$SERVER_PID" 2>/dev/null; wait "$SERVER_PID" 2>/dev/null || true

    QEMU_TEST_DIR=$(mktemp -d)
    echo "mount test content" > "$QEMU_TEST_DIR/readme.txt"
    mkdir -p "$QEMU_TEST_DIR/tools"
    echo "tool data" > "$QEMU_TEST_DIR/tools/sample.txt"
    python3 -c "print('large file line\\n' * 3000, end='')" > "$QEMU_TEST_DIR/large.txt"

    python3 "$SCRIPT_DIR/xfer-server.py" --root "$QEMU_TEST_DIR" --port $SERVER_PORT --bind 0.0.0.0 &
    SERVER_PID=$!
    sleep 0.5

    # ========================================================================
    # Mount integration tests
    # ========================================================================

    for QEMU_ARCH in "${QEMU_ARCHS[@]}"; do
        info "QEMU" "=== Mount integration tests: $QEMU_ARCH ==="

        ARCH_DIR=$(echo "$QEMU_ARCH" | tr '[:upper:]' '[:lower:]')
        [ "$ARCH_DIR" = "aarch64" ] && ARCH_DIR="aa64"
        APP_EFI="$PROJECT_ROOT/build/axl/$ARCH_DIR/axl-webfs.efi"
        DRV_EFI="$PROJECT_ROOT/build/axl/$ARCH_DIR/axl-webfs-dxe.efi"

        if [ ! -f "$APP_EFI" ] || [ ! -f "$DRV_EFI" ]; then
            skip "$QEMU_ARCH: binaries not built"
            continue
        fi

        # Copy axl-webfs as TestApp for exec-from-mount test
        cp "$APP_EFI" "$QEMU_TEST_DIR/TestApp.efi"

        # Build custom startup.nsh for mount test
        MOUNT_NSH=$(mktemp --suffix=.nsh)
        cat > "$MOUNT_NSH" <<NSHEOF
@echo -off
echo === axl-webfs Integration Test ===
fs0:
axl-webfs.efi mount http://10.0.2.2:${SERVER_PORT}/
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
axl-webfs.efi umount
echo === TESTS COMPLETE ===
reset -s
NSHEOF

        SERIAL_LOG=$(mktemp)
        info "QEMU" "$QEMU_ARCH: Booting mount test..."

        "$RUN_QEMU_SH" --arch "$QEMU_ARCH" --timeout 30 --raw --net \
            --extra "$DRV_EFI" --nsh "$MOUNT_NSH" \
            --serial-log "$SERIAL_LOG" \
            "$APP_EFI" > /dev/null 2>&1 || true

        rm -f "$MOUNT_NSH"

        if [ ! -s "$SERIAL_LOG" ]; then
            fail "$QEMU_ARCH: serial log" "no output captured"
            rm -f "$SERIAL_LOG"
            continue
        fi

        info "QEMU" "$QEMU_ARCH: Checking results..."

        if grep -q "axl-webfs" "$SERIAL_LOG" 2>/dev/null; then
            pass "$QEMU_ARCH: axl-webfs.efi executed"
        else
            fail "$QEMU_ARCH: axl-webfs.efi" "not found in serial output"
            cat "$SERIAL_LOG" | strings | head -20
            rm -f "$SERIAL_LOG"
            continue
        fi

        if grep -q "Mounted successfully" "$SERIAL_LOG" 2>/dev/null; then
            pass "$QEMU_ARCH: mount connected to xfer-server"
        else
            fail "$QEMU_ARCH: mount" "driver did not report success"
            grep -i "error\|fail\|axl-webfs-dxe" "$SERIAL_LOG" 2>/dev/null | head -5 || true
        fi

        grep -q "readme.txt" "$SERIAL_LOG" 2>/dev/null && \
            pass "$QEMU_ARCH: ls shows remote files (readme.txt)" || \
            fail "$QEMU_ARCH: ls" "readme.txt not found"

        grep -q "mount test content" "$SERIAL_LOG" 2>/dev/null && \
            pass "$QEMU_ARCH: type reads remote file content" || \
            fail "$QEMU_ARCH: type" "file content not found"

        [ -f "$QEMU_TEST_DIR/from_uefi.txt" ] && \
            pass "$QEMU_ARCH: cp wrote file to workstation" || \
            fail "$QEMU_ARCH: write" "from_uefi.txt not on workstation"

        grep -q "sample.txt" "$SERIAL_LOG" 2>/dev/null && \
            pass "$QEMU_ARCH: ls subdir shows remote files" || \
            fail "$QEMU_ARCH: ls subdir" "sample.txt not found"

        grep -qE "axl-webfs v[0-9]+\.[0-9]+" "$SERIAL_LOG" 2>/dev/null && \
            pass "$QEMU_ARCH: exec .efi from mounted volume" || \
            fail "$QEMU_ARCH: exec from mount" "axl-webfs version banner not found"

        grep -q "large file line" "$SERIAL_LOG" 2>/dev/null && \
            pass "$QEMU_ARCH: large file read (50KB)" || \
            fail "$QEMU_ARCH: large file" "content not found"

        grep -q "Unmounted" "$SERIAL_LOG" 2>/dev/null && \
            pass "$QEMU_ARCH: umount succeeded" || \
            fail "$QEMU_ARCH: umount" "Unmounted not found"

        grep -q "TESTS COMPLETE" "$SERIAL_LOG" 2>/dev/null && \
            pass "$QEMU_ARCH: all Shell commands completed" || \
            fail "$QEMU_ARCH: completion" "startup.nsh did not finish"

        rm -f "$SERIAL_LOG" "$QEMU_TEST_DIR/from_uefi.txt"
    done

    rm -rf "$QEMU_TEST_DIR"

    # ========================================================================
    # Serve integration tests (QEMU runs axl-webfs serve, host curls)
    # ========================================================================

    SERVE_PORT=18090

    for QEMU_ARCH in "${QEMU_ARCHS[@]}"; do
        info "QEMU" "=== Serve integration tests: $QEMU_ARCH ==="

        ARCH_DIR=$(echo "$QEMU_ARCH" | tr '[:upper:]' '[:lower:]')
        [ "$ARCH_DIR" = "aarch64" ] && ARCH_DIR="aa64"
        APP_EFI="$PROJECT_ROOT/build/axl/$ARCH_DIR/axl-webfs.efi"

        if [ ! -f "$APP_EFI" ]; then
            skip "$QEMU_ARCH serve: axl-webfs.efi not built"
            continue
        fi

        # Create a test file to stage on the ESP (name must match what tests expect)
        SERVE_STAGE_DIR=$(mktemp -d)
        echo "serve test file" > "$SERVE_STAGE_DIR/serve_test.txt"

        info "QEMU" "$QEMU_ARCH serve: Starting server in QEMU..."

        eval "$("$RUN_QEMU_SH" --arch "$QEMU_ARCH" --timeout 30 \
            --net --hostfwd "${SERVE_PORT}:8080" \
            --extra "$SERVE_STAGE_DIR/serve_test.txt" \
            --background \
            "$APP_EFI" serve -p 8080)"

        rm -rf "$SERVE_STAGE_DIR"

        # QEMU_PID, SERIAL_LOG, TMPDIR now set by eval
        if [ -z "${QEMU_PID:-}" ]; then
            fail "$QEMU_ARCH serve: QEMU failed to start"
            continue
        fi

        # Poll until server is ready
        READY=false
        for WAIT in $(seq 1 20); do
            sleep 1
            if ! kill -0 "$QEMU_PID" 2>/dev/null; then break; fi
            HTTP_CHECK=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:${SERVE_PORT}/" 2>/dev/null || true)
            if [ "$HTTP_CHECK" = "200" ]; then
                READY=true
                break
            fi
        done

        if ! $READY; then
            fail "$QEMU_ARCH serve: server did not start within 20s"
            kill "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null || true
            rm -rf "$TMPDIR"
            continue
        fi

        BASE="http://127.0.0.1:${SERVE_PORT}"

        RESP=$(curl -sf -H "Accept: application/json" "$BASE/" 2>/dev/null)
        echo "$RESP" | grep -q "fs0" && \
            pass "$QEMU_ARCH serve: GET / returns volume list with fs0" || \
            fail "$QEMU_ARCH serve: GET /" "no fs0 in response: $RESP"

        RESP=$(curl -sf -H "Accept: application/json" "$BASE/fs0/" 2>/dev/null)
        echo "$RESP" | grep -q "axl-webfs.efi" && \
            pass "$QEMU_ARCH serve: GET /fs0/ lists files" || \
            fail "$QEMU_ARCH serve: GET /fs0/" "axl-webfs.efi not in listing"

        CONTENT=$(curl -sf "$BASE/fs0/serve_test.txt" 2>/dev/null)
        echo "$CONTENT" | grep -q "serve test file" && \
            pass "$QEMU_ARCH serve: GET /fs0/serve_test.txt downloads file" || \
            fail "$QEMU_ARCH serve: file download" "unexpected: $CONTENT"

        HTTP_CODE=$(echo -n "upload data!" | curl -sf -o /dev/null -w "%{http_code}" \
            -T - "$BASE/fs0/uploaded.txt" 2>/dev/null)
        [ "$HTTP_CODE" = "201" ] && \
            pass "$QEMU_ARCH serve: PUT /fs0/uploaded.txt returns 201" || \
            fail "$QEMU_ARCH serve: PUT" "expected 201, got $HTTP_CODE"

        CONTENT=$(curl -sf "$BASE/fs0/uploaded.txt" 2>/dev/null)
        echo "$CONTENT" | grep -q "upload data" && \
            pass "$QEMU_ARCH serve: uploaded file readable" || \
            fail "$QEMU_ARCH serve: read upload" "content: $CONTENT"

        HTTP_CODE=$(curl -sf -o /dev/null -w "%{http_code}" -X DELETE "$BASE/fs0/uploaded.txt" 2>/dev/null)
        [ "$HTTP_CODE" = "200" ] && \
            pass "$QEMU_ARCH serve: DELETE returns 200" || \
            fail "$QEMU_ARCH serve: DELETE" "expected 200, got $HTTP_CODE"

        HTTP_CODE=$(curl -sf -o /dev/null -w "%{http_code}" -X POST "$BASE/fs0/testdir/?mkdir" 2>/dev/null)
        [ "$HTTP_CODE" = "201" ] && \
            pass "$QEMU_ARCH serve: POST ?mkdir returns 201" || \
            fail "$QEMU_ARCH serve: mkdir" "expected 201, got $HTTP_CODE"

        PARTIAL=$(curl -sf -H "Range: bytes=6-9" "$BASE/fs0/serve_test.txt" 2>/dev/null)
        RANGE_CODE=$(curl -sf -o /dev/null -w "%{http_code}" -H "Range: bytes=6-9" "$BASE/fs0/serve_test.txt" 2>/dev/null)
        [ "$RANGE_CODE" = "206" ] && [ "$PARTIAL" = "test" ] && \
            pass "$QEMU_ARCH serve: Range request returns 206 + correct bytes" || \
            fail "$QEMU_ARCH serve: Range" "code=$RANGE_CODE content='$PARTIAL'"

        HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/fs99/anything" 2>/dev/null || true)
        [ "$HTTP_CODE" = "404" ] && \
            pass "$QEMU_ARCH serve: bad volume returns 404" || \
            fail "$QEMU_ARCH serve: bad volume" "expected 404, got $HTTP_CODE"

        HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/fs0/nonexistent.xyz" 2>/dev/null || true)
        [ "$HTTP_CODE" = "404" ] && \
            pass "$QEMU_ARCH serve: nonexistent file returns 404" || \
            fail "$QEMU_ARCH serve: nonexistent" "expected 404, got $HTTP_CODE"

        kill "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null || true
        rm -rf "$TMPDIR"
    done

    # ========================================================================
    # Serve read-only mode test (X64 only)
    # ========================================================================

    info "QEMU" "=== Serve read-only mode test: X64 ==="

    APP_EFI="$PROJECT_ROOT/build/axl/x64/axl-webfs.efi"
    RO_STAGE_DIR=$(mktemp -d)
    echo "readonly test" > "$RO_STAGE_DIR/ro_test.txt"

    eval "$("$RUN_QEMU_SH" --arch X64 --timeout 30 \
        --net --hostfwd "${SERVE_PORT}:8080" \
        --extra "$RO_STAGE_DIR/ro_test.txt" \
        --background \
        "$APP_EFI" serve -p 8080 --read-only)"

    rm -rf "$RO_STAGE_DIR"

    if [ -n "${QEMU_PID:-}" ]; then
        READY=false
        for WAIT in $(seq 1 20); do
            sleep 1
            if ! kill -0 "$QEMU_PID" 2>/dev/null; then break; fi
            HTTP_CHECK=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:${SERVE_PORT}/" 2>/dev/null || true)
            if [ "$HTTP_CHECK" = "200" ]; then
                READY=true; break
            fi
        done

        BASE="http://127.0.0.1:${SERVE_PORT}"

        if $READY; then
            CONTENT=$(curl -sf "$BASE/fs0/ro_test.txt" 2>/dev/null)
            echo "$CONTENT" | grep -q "readonly test" && \
                pass "serve --read-only: GET works" || \
                fail "serve --read-only: GET" "unexpected: $CONTENT"

            HTTP_CODE=$(echo -n "blocked" | curl -s -o /dev/null -w "%{http_code}" -T - "$BASE/fs0/blocked.txt" 2>/dev/null || true)
            [ "$HTTP_CODE" = "403" ] && \
                pass "serve --read-only: PUT blocked (403)" || \
                fail "serve --read-only: PUT" "expected 403, got $HTTP_CODE"

            HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X DELETE "$BASE/fs0/ro_test.txt" 2>/dev/null || true)
            [ "$HTTP_CODE" = "403" ] && \
                pass "serve --read-only: DELETE blocked (403)" || \
                fail "serve --read-only: DELETE" "expected 403, got $HTTP_CODE"
        else
            fail "serve --read-only: server start" "did not start within 20s"
        fi

        kill "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null || true
        rm -rf "$TMPDIR"
    else
        fail "serve --read-only: QEMU start" "failed to launch"
    fi

    fi  # run-qemu.sh exists
fi

# ============================================================================
# Summary
# ============================================================================

echo ""
echo "=========================================="
echo "       axl-webfs Test Summary"
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
