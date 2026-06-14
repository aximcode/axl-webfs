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
# Embedded asset: validate that upload-asset.c's upload_js[] parses as
# valid JavaScript. The QEMU tests verify the asset is served and that
# its body contains expected hooks, but a syntax error inside the
# string literal would only manifest in a real browser; this catches
# it on the host before we ship a broken .efi.
# ============================================================================

info "ASSET" "Validate embedded upload.js"

ASSET_C="$PROJECT_ROOT/src/serve/upload-asset.c"
if [ ! -f "$ASSET_C" ]; then
    fail "asset source missing" "$ASSET_C not found"
elif ! command -v node >/dev/null 2>&1; then
    skip "node not installed (cannot run --check on embedded JS)"
else
    EXTRACTED_JS=$(mktemp --suffix=.js)
    python3 - "$ASSET_C" "$EXTRACTED_JS" <<'PYEOF'
import re, sys
src = open(sys.argv[1]).read()
m = re.search(r'upload_js\[\]\s*=\s*([\s\S]+?);\s*$', src, re.M)
if not m:
    sys.exit('upload_js declaration not found')
body = m.group(1)
# Match each "..." literal, honoring escaped quotes.
parts = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
js = ''.join(parts).encode().decode('unicode_escape')
open(sys.argv[2], 'w').write(js)
PYEOF
    if [ -s "$EXTRACTED_JS" ] && node --check "$EXTRACTED_JS" 2>/dev/null; then
        pass "embedded upload.js parses as valid JavaScript"
    else
        SYNTAX_ERR=$(node --check "$EXTRACTED_JS" 2>&1 | head -2)
        fail "embedded upload.js" "node --check failed: $SYNTAX_ERR"
    fi
    rm -f "$EXTRACTED_JS"
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

    # Non-ASCII filename regression fixture — exercises the
    # SDK's UCS-2/UTF-8 thunk on EFI_FILE_INFO trailer round-trip.
    # Pre-Phase-C `axl_utf8_to_ucs2_buf` was a Latin-1 cast that
    # smeared multi-byte UTF-8 across CHAR16 cells; with the Phase-C
    # fix the entries should appear in the Shell `ls` listing
    # byte-for-byte. We assert via a server-side request log probe
    # (mount client GETs /list/ and the entries it sees in the JSON
    # response have the correct names) plus a serial-log probe
    # against the Latin-1 byte produced by OVMF when it prints the
    # CHAR16 'é' (U+00E9 → byte 0xE9 in the serial console).
    python3 -c "
import sys
open(sys.argv[1] + '/résumé.txt', 'w').write('non-ascii body\n')
open(sys.argv[1] + '/日本語.bin', 'wb').write(b'JP\n')
" "$QEMU_TEST_DIR"

    # Multi-chunk PUT regression source: a deterministic-content
    # ~256 KB file lives on the ESP and gets cp'd to fs1: via the
    # mount. UEFI Shell's `cp` chunks anything past its internal
    # 64 KB read at minimum; before WebFsWrite buffered, each Write
    # produced a fresh PUT-overwrite and the destination ended up
    # containing only the last chunk. The post-QEMU md5 compare
    # pins byte-for-byte correctness.
    MULTI_CHUNK_SRC=$(mktemp --suffix=.bin)
    python3 -c "import sys; sys.stdout.buffer.write((b'multichunk regression payload - ' * 8192)[:262144])" > "$MULTI_CHUNK_SRC"
    MULTI_CHUNK_MD5=$(md5sum "$MULTI_CHUNK_SRC" | awk '{print $1}')

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

        # The mount driver is .incbin'd into axl-webfs.efi via
        # axl-cc --embed, so no sidecar driver staging is required.
        if [ ! -f "$APP_EFI" ]; then
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
echo === TEST: multi-chunk PUT ===
cp fs0:\\multichunk.bin fs1:\\multichunk-uploaded.bin
echo === TEST: read subdir ===
ls fs1:\\tools\\
echo === TEST: mkdir ===
mkdir fs1:\\new-test-dir
ls fs1:\\
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

        # Stage the multi-chunk source on the ESP so the NSH's
        # `cp fs0:\multichunk.bin fs1:\multichunk-uploaded.bin`
        # round-trips through the mount client. --extra files end
        # up at the ESP root with their basename, hence
        # fs0:\multichunk.bin.
        MULTI_CHUNK_STAGE="$(dirname "$MULTI_CHUNK_SRC")/multichunk.bin"
        cp "$MULTI_CHUNK_SRC" "$MULTI_CHUNK_STAGE"
        "$RUN_QEMU_SH" --arch "$QEMU_ARCH" --timeout 30 --raw --net \
            --nsh "$MOUNT_NSH" \
            --extra "$MULTI_CHUNK_STAGE" \
            --serial-log "$SERIAL_LOG" \
            "$APP_EFI" > /dev/null 2>&1 || true
        rm -f "$MULTI_CHUNK_STAGE"

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

        if grep -qE "axl-webfs-mount: mounted" "$SERIAL_LOG" 2>/dev/null; then
            pass "$QEMU_ARCH: mount connected to xfer-server"
        else
            fail "$QEMU_ARCH: mount" "driver did not report success"
            grep -i "error\|fail\|axl-webfs-mount" "$SERIAL_LOG" 2>/dev/null | head -5 || true
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

        # Multi-chunk PUT regression check: compare the uploaded file's
        # md5 against the source. UEFI Shell's cp internally chunks
        # large files; without WebFsWrite's staging buffer this would
        # arrive at the workstation containing only the last chunk's
        # bytes (mismatch, fail). With the buffer the bytes compose
        # into a single PUT and md5 matches.
        if [ -f "$QEMU_TEST_DIR/multichunk-uploaded.bin" ]; then
            GOT_MD5=$(md5sum "$QEMU_TEST_DIR/multichunk-uploaded.bin" \
                      | awk '{print $1}')
            if [ "$GOT_MD5" = "$MULTI_CHUNK_MD5" ]; then
                pass "$QEMU_ARCH: multi-chunk PUT round-trips (md5 match)"
            else
                fail "$QEMU_ARCH: multi-chunk PUT" \
                     "md5 mismatch: got $GOT_MD5, want $MULTI_CHUNK_MD5"
            fi
        else
            fail "$QEMU_ARCH: multi-chunk PUT" \
                 "multichunk-uploaded.bin not on workstation"
        fi

        grep -q "sample.txt" "$SERIAL_LOG" 2>/dev/null && \
            pass "$QEMU_ARCH: ls subdir shows remote files" || \
            fail "$QEMU_ARCH: ls subdir" "sample.txt not found"

        grep -qE "axl-webfs.*v[0-9]+\.[0-9]+" "$SERIAL_LOG" 2>/dev/null && \
            pass "$QEMU_ARCH: exec .efi from mounted volume" || \
            fail "$QEMU_ARCH: exec from mount" "axl-webfs version banner not found"

        grep -q "large file line" "$SERIAL_LOG" 2>/dev/null && \
            pass "$QEMU_ARCH: large file read (50KB)" || \
            fail "$QEMU_ARCH: large file" "content not found"

        # Non-ASCII filename round-trip — the SDK's Phase-C
        # axl_utf8_to_ucs2_buf decoder fix must propagate
        # 'résumé.txt' and '日本語.bin' from the JSON listing
        # through EFI_FILE_INFO trailers into Shell ls output
        # without the pre-fix Latin-1-cast smearing.
        #
        # OVMF's serial console driver renders unprintable CHAR16
        # cells as the ASCII string `uXXXX` (no backslash) — so a
        # *correctly* round-tripped 'é' (U+00E9) lands as the
        # 5-byte ASCII fragment `u00e9` in the captured serial,
        # surrounded by neighboring file-name characters. The
        # regression we're guarding against (Latin-1 cast of the
        # UTF-8 bytes 0xC3 0xA9) would instead produce two cells
        # 0x00C3 + 0x00A9 → `u00c3u00a9` — present iff the bug is
        # back. Grep both directions: positive for `ru00e9sum` and
        # `u65e5u672cu8a9e`, negative for `u00c3u00a9`.
        if grep -q 'ru00e9sumu00e9' "$SERIAL_LOG" 2>/dev/null; then
            pass "$QEMU_ARCH: non-ASCII 'résumé.txt' (CHAR16 U+00E9) round-trip"
        else
            fail "$QEMU_ARCH: non-ASCII résumé.txt" \
                 "expected 'ru00e9sumu00e9' fragment in ls output"
        fi
        if grep -q 'u65e5u672cu8a9e' "$SERIAL_LOG" 2>/dev/null; then
            pass "$QEMU_ARCH: non-ASCII '日本語.bin' (3-byte UTF-8) round-trip"
        else
            fail "$QEMU_ARCH: non-ASCII 日本語.bin" \
                 "expected 'u65e5u672cu8a9e' fragment in ls output"
        fi
        if grep -q 'u00c3u00a9' "$SERIAL_LOG" 2>/dev/null; then
            fail "$QEMU_ARCH: UTF-8 Latin-1 cast regression" \
                 "saw 'u00c3u00a9' — axl_utf8_to_ucs2_buf reverted to byte cast?"
        else
            pass "$QEMU_ARCH: no Latin-1-cast smearing (no 'u00c3u00a9')"
        fi

        # mkdir test — the AxlFsProviderOpen `attributes` arg
        # (added in Phase C) lets Shell mkdir reach the provider's
        # mkdir wire path. Pre-Phase-C this was impossible from
        # the EFI side. Workstation-side check: the directory
        # appears on the server.
        if [ -d "$QEMU_TEST_DIR/new-test-dir" ]; then
            pass "$QEMU_ARCH: Shell mkdir creates directory on server"
        else
            fail "$QEMU_ARCH: mkdir" \
                 "new-test-dir not present on workstation — provider mkdir not invoked?"
        fi

        grep -qE "axl-webfs-mount: unmounted" "$SERIAL_LOG" 2>/dev/null && \
            pass "$QEMU_ARCH: umount succeeded" || \
            fail "$QEMU_ARCH: umount" "axl-webfs-mount: unmounted not found"

        grep -q "TESTS COMPLETE" "$SERIAL_LOG" 2>/dev/null && \
            pass "$QEMU_ARCH: all Shell commands completed" || \
            fail "$QEMU_ARCH: completion" "startup.nsh did not finish"

        rm -f "$SERIAL_LOG" "$QEMU_TEST_DIR/from_uefi.txt" \
              "$QEMU_TEST_DIR/multichunk-uploaded.bin"
        rm -rf "$QEMU_TEST_DIR/new-test-dir"
    done

    rm -rf "$QEMU_TEST_DIR"
    rm -f "$MULTI_CHUNK_SRC"

    # ========================================================================
    # Basic-Auth mount test
    #
    # xfer-server.py launched with --basic-auth requires the mount
    # client to send Authorization: Basic <b64>. mount --auth
    # basic:user:token sets header.Authorization on the AxlHttpClient
    # so every probe / list / read / write request inherits it.
    # ========================================================================

    kill $SERVER_PID 2>/dev/null || true
    wait $SERVER_PID 2>/dev/null || true

    info "QEMU" "=== Mount with HTTP Basic Auth ==="

    AUTH_PORT=$((SERVER_PORT + 2))
    AUTH_TEST_DIR=$(mktemp -d)
    echo "auth gated content" > "$AUTH_TEST_DIR/secret.txt"

    python3 "$SCRIPT_DIR/xfer-server.py" \
        --root "$AUTH_TEST_DIR" --port $AUTH_PORT --bind 0.0.0.0 \
        --basic-auth alice:s3cret \
        >/tmp/xfer-auth.log 2>&1 &
    AUTH_PID=$!
    sleep 1

    APP_EFI="$PROJECT_ROOT/build/axl/x64/axl-webfs.efi"
    if [ -f "$APP_EFI" ]; then
        # First: confirm the server *does* reject anonymous mount.
        AUTH_NSH=$(mktemp --suffix=.nsh)
        cat > "$AUTH_NSH" <<NSHEOF
@echo -off
fs0:
echo === ANON MOUNT (should fail) ===
axl-webfs.efi mount http://10.0.2.2:${AUTH_PORT}/
echo === AUTH MOUNT (should succeed) ===
axl-webfs.efi mount --auth basic:alice:s3cret http://10.0.2.2:${AUTH_PORT}/
map -r
echo === AUTH LS ===
ls fs1:\\
echo === AUTH TYPE ===
type fs1:\\secret.txt
echo === UMOUNT ===
axl-webfs.efi umount
echo === TESTS COMPLETE ===
reset -s
NSHEOF

        SERIAL_LOG=$(mktemp)
        info "QEMU" "X64 auth: Booting mount test..."
        "$RUN_QEMU_SH" --arch X64 --timeout 30 --raw --net \
            --nsh "$AUTH_NSH" \
            --serial-log "$SERIAL_LOG" \
            "$APP_EFI" > /dev/null 2>&1 || true
        rm -f "$AUTH_NSH"

        grep -qE "no .* endpoint reachable|mount failed" \
            "$SERIAL_LOG" 2>/dev/null && \
            pass "auth: anonymous mount rejected (server demands 401)" || \
            fail "auth: anonymous" "expected mount-failed line"

        # Same banner the existing tests use — proves --auth let us
        # past the 401 and the rest of the mount succeeded.
        grep -qE "axl-webfs-mount: mounted" \
            "$SERIAL_LOG" 2>/dev/null && \
            pass "auth: authenticated mount succeeded" || \
            fail "auth: authenticated mount" "no 'mounted' line"

        grep -q "secret.txt" "$SERIAL_LOG" 2>/dev/null && \
            pass "auth: ls fs1 shows secret.txt (listing auth'd)" || \
            fail "auth: ls" "secret.txt not in listing"

        grep -q "auth gated content" "$SERIAL_LOG" 2>/dev/null && \
            pass "auth: type fs1:\\secret.txt returns gated content" || \
            fail "auth: type" "gated content not found"

        rm -f "$SERIAL_LOG"
    else
        skip "auth: axl-webfs.efi not built"
    fi

    kill $AUTH_PID 2>/dev/null || true
    wait $AUTH_PID 2>/dev/null || true
    rm -rf "$AUTH_TEST_DIR"

    # ========================================================================
    # WebDAV mount test (parallel-protocol parity)
    #
    # Same shape as the JSON mount test above, but the workstation
    # serves WebDAV (xfer-server.py --webdav, wsgidav-backed) and the
    # mount driver picks the DAV path either by --protocol auto's
    # OPTIONS DAV: header detection or an explicit --protocol dav.
    # Gated on wsgidav being importable; skipped otherwise.
    # ========================================================================

    kill $SERVER_PID 2>/dev/null || true
    wait $SERVER_PID 2>/dev/null || true

    if python3 -c "import wsgidav, cheroot" 2>/dev/null; then
        info "QEMU" "=== Mount via WebDAV protocol ==="

        DAV_PORT=$((SERVER_PORT + 1))
        DAV_TEST_DIR=$(mktemp -d)
        echo "webdav mount test content" > "$DAV_TEST_DIR/readme.txt"
        mkdir -p "$DAV_TEST_DIR/tools"
        echo "tool data via dav" > "$DAV_TEST_DIR/tools/sample.txt"

        python3 "$SCRIPT_DIR/xfer-server.py" --webdav \
            --root "$DAV_TEST_DIR" --port $DAV_PORT --bind 0.0.0.0 \
            >/tmp/xfer-dav.log 2>&1 &
        DAV_PID=$!
        sleep 1

        for QEMU_ARCH in "${QEMU_ARCHS[@]}"; do
            ARCH_DIR=$(echo "$QEMU_ARCH" | tr '[:upper:]' '[:lower:]')
            [ "$ARCH_DIR" = "aarch64" ] && ARCH_DIR="aa64"
            APP_EFI="$PROJECT_ROOT/build/axl/$ARCH_DIR/axl-webfs.efi"
            if [ ! -f "$APP_EFI" ]; then continue; fi

            cp "$APP_EFI" "$DAV_TEST_DIR/TestApp.efi"

            DAV_NSH=$(mktemp --suffix=.nsh)
            cat > "$DAV_NSH" <<NSHEOF
@echo -off
echo === axl-webfs WebDAV Mount Test ===
fs0:
axl-webfs.efi mount --protocol dav http://10.0.2.2:${DAV_PORT}/
map -r
echo === TEST: ls mounted volume ===
ls fs1:\\
echo === TEST: read file ===
type fs1:\\readme.txt
echo === TEST: write file ===
echo "written via dav" > fs0:\\dav_write.txt
cp fs0:\\dav_write.txt fs1:\\from_uefi_dav.txt
echo === TEST: read subdir ===
ls fs1:\\tools\\
echo === TEST: umount ===
axl-webfs.efi umount
echo === TESTS COMPLETE ===
reset -s
NSHEOF

            SERIAL_LOG=$(mktemp)
            info "QEMU" "$QEMU_ARCH dav: Booting mount test..."
            "$RUN_QEMU_SH" --arch "$QEMU_ARCH" --timeout 30 --raw --net \
                --nsh "$DAV_NSH" \
                --serial-log "$SERIAL_LOG" \
                "$APP_EFI" > /dev/null 2>&1 || true
            rm -f "$DAV_NSH"

            if [ ! -s "$SERIAL_LOG" ]; then
                fail "$QEMU_ARCH dav: serial log" "no output captured"
                rm -f "$SERIAL_LOG"
                continue
            fi

            grep -qE "axl-webfs-mount: wire protocol = WebDAV" \
                "$SERIAL_LOG" 2>/dev/null && \
                pass "$QEMU_ARCH dav: mount selected WebDAV protocol" || \
                fail "$QEMU_ARCH dav: protocol selection" \
                     "no 'wire protocol = WebDAV' line in serial log"

            grep -qE "axl-webfs-mount: mounted" "$SERIAL_LOG" 2>/dev/null && \
                pass "$QEMU_ARCH dav: mount connected to xfer-server (WebDAV)" || \
                fail "$QEMU_ARCH dav: mount" "driver did not report success"

            grep -q "readme.txt" "$SERIAL_LOG" 2>/dev/null && \
                pass "$QEMU_ARCH dav: PROPFIND listing shows readme.txt" || \
                fail "$QEMU_ARCH dav: PROPFIND" "readme.txt not found"

            grep -q "webdav mount test content" "$SERIAL_LOG" 2>/dev/null && \
                pass "$QEMU_ARCH dav: GET reads remote file content" || \
                fail "$QEMU_ARCH dav: GET" "file content not found"

            [ -f "$DAV_TEST_DIR/from_uefi_dav.txt" ] && \
                pass "$QEMU_ARCH dav: PUT wrote file to workstation" || \
                fail "$QEMU_ARCH dav: PUT" "from_uefi_dav.txt not on workstation"

            grep -q "sample.txt" "$SERIAL_LOG" 2>/dev/null && \
                pass "$QEMU_ARCH dav: subdir PROPFIND works" || \
                fail "$QEMU_ARCH dav: subdir" "sample.txt not found"

            grep -qE "axl-webfs-mount: unmounted" \
                "$SERIAL_LOG" 2>/dev/null && \
                pass "$QEMU_ARCH dav: umount succeeded" || \
                fail "$QEMU_ARCH dav: umount" "no unmount line"

            rm -f "$SERIAL_LOG" "$DAV_TEST_DIR/from_uefi_dav.txt"
        done

        kill $DAV_PID 2>/dev/null || true
        wait $DAV_PID 2>/dev/null || true
        rm -rf "$DAV_TEST_DIR"
    else
        skip "WebDAV mount tests: wsgidav/cheroot not installed (pip install wsgidav cheroot)"
    fi

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

        # Upload integrity: a matching "Content-Digest: sha-256=<hex>"
        # accepts; a wrong one is rejected 400 and the partial removed.
        # Same header/format/status the SDK's /dav PUT uses.
        DG_BODY="integrity check body"
        DG_HEX=$(printf '%s' "$DG_BODY" | sha256sum | awk '{print $1}')
        HTTP_CODE=$(printf '%s' "$DG_BODY" | curl -s -o /dev/null -w "%{http_code}" \
            -H "Content-Digest: sha-256=$DG_HEX" -T - "$BASE/fs0/digest_ok.txt" 2>/dev/null)
        [ "$HTTP_CODE" = "201" ] && \
            pass "$QEMU_ARCH serve: PUT with matching Content-Digest returns 201" || \
            fail "$QEMU_ARCH serve: Content-Digest match" "expected 201, got $HTTP_CODE"

        HTTP_CODE=$(printf '%s' "$DG_BODY" | curl -s -o /dev/null -w "%{http_code}" \
            -H "Content-Digest: sha-256=0000000000000000000000000000000000000000000000000000000000000000" \
            -T - "$BASE/fs0/digest_bad.txt" 2>/dev/null)
        [ "$HTTP_CODE" = "400" ] && \
            pass "$QEMU_ARCH serve: PUT with wrong Content-Digest rejected (400)" || \
            fail "$QEMU_ARCH serve: Content-Digest mismatch" "expected 400, got $HTTP_CODE"

        # The rejected upload must not have landed.
        HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/fs0/digest_bad.txt" 2>/dev/null || true)
        [ "$HTTP_CODE" = "404" ] && \
            pass "$QEMU_ARCH serve: rejected upload left no partial file" || \
            fail "$QEMU_ARCH serve: partial cleanup" "expected 404, got $HTTP_CODE"

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

        # Upload UI smoke tests: asset endpoint + HTML wiring.
        # Use GET (-D - dumps headers); HEAD isn't registered for this route.
        ASSET_HEADERS=$(curl -s -D - -o /dev/null "$BASE/_axl-webfs/upload.js" 2>/dev/null)
        echo "$ASSET_HEADERS" | grep -qE "^HTTP/[0-9.]+ 200" && \
        echo "$ASSET_HEADERS" | grep -qiE "^content-type:.*application/javascript" && \
            pass "$QEMU_ARCH serve: upload.js asset returns 200 + JS content-type" || \
            fail "$QEMU_ARCH serve: upload.js asset" "$(echo "$ASSET_HEADERS" | head -2)"

        ASSET_BODY=$(curl -sf "$BASE/_axl-webfs/upload.js" 2>/dev/null)
        echo "$ASSET_BODY" | grep -q "location.pathname" && \
        echo "$ASSET_BODY" | grep -q "XMLHttpRequest" && \
        echo "$ASSET_BODY" | grep -q "dropzone-active" && \
            pass "$QEMU_ARCH serve: upload.js body has expected hooks" || \
            fail "$QEMU_ARCH serve: upload.js body" "missing pathname/XHR/dropzone tokens"

        DIR_HTML=$(curl -sf "$BASE/fs0/" 2>/dev/null)
        echo "$DIR_HTML" | grep -q 'id="axl-upload-btn"' && \
        echo "$DIR_HTML" | grep -q '<title>fs0:/</title>' && \
        echo "$DIR_HTML" | grep -q 'src="/_axl-webfs/upload.js"' && \
            pass "$QEMU_ARCH serve: dir HTML wires upload button + title + script" || \
            fail "$QEMU_ARCH serve: dir HTML wiring" "missing button/title/script tags"

        # Nested-path title: catches a future format-string-arg reorder
        # bug (vol_esc, path_esc -> typo). The testdir/ subdir was
        # created by the POST ?mkdir test above.
        NESTED_HTML=$(curl -sf "$BASE/fs0/testdir/" 2>/dev/null)
        echo "$NESTED_HTML" | grep -q '<title>fs0:/testdir/</title>' && \
            pass "$QEMU_ARCH serve: nested-path title correct" || \
            fail "$QEMU_ARCH serve: nested title" "missing /testdir/ in title"

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
        "$APP_EFI" serve -p 8080 --mode read-only)"

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

            # In read-only mode the upload UI must be omitted from the
            # listing (no button, no script tag, no upload-related CSS).
            RO_HTML=$(curl -sf "$BASE/fs0/" 2>/dev/null)
            ! echo "$RO_HTML" | grep -q 'id="axl-upload-btn"' && \
            ! echo "$RO_HTML" | grep -q 'src="/_axl-webfs/upload.js"' && \
            ! echo "$RO_HTML" | grep -q 'dropzone-active' && \
                pass "serve --read-only: upload UI omitted from HTML" || \
                fail "serve --read-only: upload UI" "button/script/dropzone leaked into read-only listing"
        else
            fail "serve --read-only: server start" "did not start within 20s"
        fi

        kill "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null || true
        rm -rf "$TMPDIR"
    else
        fail "serve --read-only: QEMU start" "failed to launch"
    fi

    # ========================================================================
    # Serve write-only mode test (X64 only)
    # ========================================================================

    info "QEMU" "=== Serve write-only mode test: X64 ==="

    APP_EFI="$PROJECT_ROOT/build/axl/x64/axl-webfs.efi"

    eval "$("$RUN_QEMU_SH" --arch X64 --timeout 30 \
        --net --hostfwd "${SERVE_PORT}:8080" \
        --background \
        "$APP_EFI" serve -p 8080 --mode write-only)"

    if [ -n "${QEMU_PID:-}" ]; then
        # In write-only mode GETs return 403, so the readiness probe waits
        # for any HTTP response from the listener (403 means server is up).
        READY=false
        for WAIT in $(seq 1 20); do
            sleep 1
            if ! kill -0 "$QEMU_PID" 2>/dev/null; then break; fi
            HTTP_CHECK=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:${SERVE_PORT}/" 2>/dev/null || true)
            if [ "$HTTP_CHECK" = "403" ]; then
                READY=true; break
            fi
        done

        BASE="http://127.0.0.1:${SERVE_PORT}"

        if $READY; then
            HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/fs0/" 2>/dev/null || true)
            [ "$HTTP_CODE" = "403" ] && \
                pass "serve --mode write-only: GET blocked (403)" || \
                fail "serve --mode write-only: GET" "expected 403, got $HTTP_CODE"

            HTTP_CODE=$(echo -n "wo upload" | curl -s -o /dev/null -w "%{http_code}" -T - "$BASE/fs0/wo_test.txt" 2>/dev/null || true)
            [ "$HTTP_CODE" = "201" ] && \
                pass "serve --mode write-only: PUT works (201)" || \
                fail "serve --mode write-only: PUT" "expected 201, got $HTTP_CODE"

            HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE/fs0/wodir/?mkdir" 2>/dev/null || true)
            [ "$HTTP_CODE" = "201" ] && \
                pass "serve --mode write-only: POST ?mkdir works (201)" || \
                fail "serve --mode write-only: mkdir" "expected 201, got $HTTP_CODE"
        else
            fail "serve --mode write-only: server start" "did not start within 20s"
        fi

        kill "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null || true
        rm -rf "$TMPDIR"
    else
        fail "serve --mode write-only: QEMU start" "failed to launch"
    fi

    # ========================================================================
    # Serve HTTP Basic auth test (X64 only) — --auth gates every surface
    # (REST + uploads + /dav) via the SDK's add_*_auth registrations.
    # A 401 carries WWW-Authenticate: Basic (set_auth_challenge), so
    # interactive clients prompt; here we drive it with curl -u.
    # ========================================================================

    info "QEMU" "=== Serve auth test: X64 ==="

    APP_EFI="$PROJECT_ROOT/build/axl/x64/axl-webfs.efi"
    AUTH_STAGE_DIR=$(mktemp -d)
    echo "auth test file" > "$AUTH_STAGE_DIR/auth_test.txt"

    eval "$("$RUN_QEMU_SH" --arch X64 --timeout 30 \
        --net --hostfwd "${SERVE_PORT}:8080" \
        --extra "$AUTH_STAGE_DIR/auth_test.txt" \
        --background \
        "$APP_EFI" serve -p 8080 --auth admin:s3cret)"

    rm -rf "$AUTH_STAGE_DIR"

    if [ -n "${QEMU_PID:-}" ]; then
        # Gated server answers 401 to anonymous GET — use that as the
        # readiness signal (a 200 never comes without credentials).
        READY=false
        for WAIT in $(seq 1 20); do
            sleep 1
            if ! kill -0 "$QEMU_PID" 2>/dev/null; then break; fi
            HTTP_CHECK=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:${SERVE_PORT}/" 2>/dev/null || true)
            if [ "$HTTP_CHECK" = "401" ]; then
                READY=true; break
            fi
        done

        BASE="http://127.0.0.1:${SERVE_PORT}"

        if $READY; then
            ANON_HEADERS=$(curl -s -D - -o /dev/null "$BASE/" 2>/dev/null || true)
            echo "$ANON_HEADERS" | grep -qE "^HTTP/[0-9.]+ 401" && \
                pass "serve --auth: anonymous GET / rejected (401)" || \
                fail "serve --auth: anon GET" "expected 401, got $(echo "$ANON_HEADERS" | head -1)"

            # The 401 must carry a WWW-Authenticate: Basic challenge so
            # interactive clients prompt (set_auth_challenge).
            echo "$ANON_HEADERS" | grep -qiE '^WWW-Authenticate:[[:space:]]*Basic([[:space:]]|$)' && \
                pass "serve --auth: 401 carries WWW-Authenticate: Basic" || \
                fail "serve --auth: WWW-Authenticate" "missing/unexpected: $(echo "$ANON_HEADERS" | grep -i www-authenticate)"

            HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -u wrong:pass "$BASE/fs0/" 2>/dev/null || true)
            [ "$HTTP_CODE" = "401" ] && \
                pass "serve --auth: wrong credential rejected (401)" || \
                fail "serve --auth: wrong cred" "expected 401, got $HTTP_CODE"

            CONTENT=$(curl -sf -u admin:s3cret "$BASE/fs0/auth_test.txt" 2>/dev/null || true)
            echo "$CONTENT" | grep -q "auth test file" && \
                pass "serve --auth: authed GET works" || \
                fail "serve --auth: authed GET" "unexpected: $CONTENT"

            HTTP_CODE=$(echo -n "authed upload" | curl -s -o /dev/null -w "%{http_code}" \
                -u admin:s3cret -T - "$BASE/fs0/authed.txt" 2>/dev/null || true)
            [ "$HTTP_CODE" = "201" ] && \
                pass "serve --auth: authed PUT works (201)" || \
                fail "serve --auth: authed PUT" "expected 201, got $HTTP_CODE"

            # Streaming uploads bypass the dispatch-time auth check, so a
            # PUT with no credentials must still be refused (401) at the
            # early upload site — the regression the SDK _auth upload
            # route closes.
            HTTP_CODE=$(echo -n "anon upload" | curl -s -o /dev/null -w "%{http_code}" \
                -T - "$BASE/fs0/anon.txt" 2>/dev/null || true)
            [ "$HTTP_CODE" = "401" ] && \
                pass "serve --auth: anonymous PUT rejected (401)" || \
                fail "serve --auth: anon PUT" "expected 401, got $HTTP_CODE"

            # /dav mount is gated uniformly: anon PROPFIND 401, authed 207.
            HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X PROPFIND \
                -H "Depth: 1" "$BASE/dav/fs0/" 2>/dev/null || true)
            [ "$HTTP_CODE" = "401" ] && \
                pass "serve --auth: anonymous /dav PROPFIND rejected (401)" || \
                fail "serve --auth: anon /dav" "expected 401, got $HTTP_CODE"

            HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -u admin:s3cret -X PROPFIND \
                -H "Depth: 1" "$BASE/dav/fs0/" 2>/dev/null || true)
            [ "$HTTP_CODE" = "207" ] && \
                pass "serve --auth: authed /dav PROPFIND works (207)" || \
                fail "serve --auth: authed /dav" "expected 207, got $HTTP_CODE"
        else
            fail "serve --auth: server start" "did not start within 20s"
        fi

        kill "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null || true
        rm -rf "$TMPDIR"
    else
        fail "serve --auth: QEMU start" "failed to launch"
    fi

    # ========================================================================
    # Serve auth footgun: a colon-less --auth value can never match a
    # Basic credential, so the foreground handler must reject it at parse
    # time (before launching the driver) rather than start a server that
    # locks out every client. We boot once and assert the error message
    # reaches the serial console.
    # ========================================================================

    info "QEMU" "=== Serve auth footgun test: X64 ==="

    APP_EFI="$PROJECT_ROOT/build/axl/x64/axl-webfs.efi"
    REJECT_NSH=$(mktemp --suffix=.nsh)
    cat > "$REJECT_NSH" <<'NSHEOF'
@echo -off
fs0:
axl-webfs.efi serve -p 8080 --auth nocolon
echo === AUTH-REJECT-DONE ===
reset -s
NSHEOF
    REJECT_LOG=$(mktemp)
    "$RUN_QEMU_SH" --arch X64 --timeout 30 \
        --nsh "$REJECT_NSH" \
        --serial-log "$REJECT_LOG" \
        "$APP_EFI" > /dev/null 2>&1 || true
    rm -f "$REJECT_NSH"

    if grep -q "auth must be" "$REJECT_LOG" 2>/dev/null; then
        pass "serve --auth: colon-less value rejected at parse"
    else
        fail "serve --auth: colon-less reject" "no error message in serial log"
    fi
    rm -f "$REJECT_LOG"

    # ========================================================================
    # Serve HTTPS test (X64 only) — --tls serves over a self-signed cert.
    # Combined with --auth to exercise the secure combo (credentials no
    # longer cross the wire in cleartext). curl -k accepts the self-signed
    # cert; the gate still answers 401 + WWW-Authenticate over TLS.
    # ========================================================================

    info "QEMU" "=== Serve HTTPS test: X64 ==="

    APP_EFI="$PROJECT_ROOT/build/axl/x64/axl-webfs.efi"
    TLS_STAGE_DIR=$(mktemp -d)
    echo "tls test file" > "$TLS_STAGE_DIR/tls_test.txt"

    eval "$("$RUN_QEMU_SH" --arch X64 --timeout 30 \
        --net --hostfwd "${SERVE_PORT}:8080" \
        --extra "$TLS_STAGE_DIR/tls_test.txt" \
        --background \
        "$APP_EFI" serve -p 8080 --tls --auth admin:s3cret)"

    rm -rf "$TLS_STAGE_DIR"

    if [ -n "${QEMU_PID:-}" ]; then
        # HTTPS + gated: an anon GET over TLS answers 401 once up.
        READY=false
        for WAIT in $(seq 1 20); do
            sleep 1
            if ! kill -0 "$QEMU_PID" 2>/dev/null; then break; fi
            HTTP_CHECK=$(curl -sk -o /dev/null -w "%{http_code}" "https://127.0.0.1:${SERVE_PORT}/" 2>/dev/null || true)
            if [ "$HTTP_CHECK" = "401" ]; then
                READY=true; break
            fi
        done

        BASE="https://127.0.0.1:${SERVE_PORT}"

        if $READY; then
            # TLS handshake completed (curl -k) and the gate responded.
            ANON_HEADERS=$(curl -sk -D - -o /dev/null "$BASE/" 2>/dev/null || true)
            echo "$ANON_HEADERS" | grep -qE "^HTTP/[0-9.]+ 401" && \
            echo "$ANON_HEADERS" | grep -qiE '^WWW-Authenticate:[[:space:]]*Basic' && \
                pass "serve --tls: HTTPS handshake + gated 401 + challenge" || \
                fail "serve --tls: anon HTTPS" "$(echo "$ANON_HEADERS" | head -1)"

            CONTENT=$(curl -sk -u admin:s3cret "$BASE/fs0/tls_test.txt" 2>/dev/null || true)
            echo "$CONTENT" | grep -q "tls test file" && \
                pass "serve --tls: authed HTTPS GET works" || \
                fail "serve --tls: authed HTTPS GET" "unexpected: $CONTENT"

            # A plaintext HTTP request to the TLS port must not succeed.
            HTTP_CODE=$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" "http://127.0.0.1:${SERVE_PORT}/" 2>/dev/null || true)
            [ "$HTTP_CODE" != "200" ] && \
                pass "serve --tls: plaintext HTTP to TLS port refused" || \
                fail "serve --tls: cleartext" "plain HTTP unexpectedly returned 200"
        else
            fail "serve --tls: server start" "did not answer HTTPS within 20s"
        fi

        kill "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null || true
        rm -rf "$TMPDIR"
    else
        fail "serve --tls: QEMU start" "failed to launch"
    fi

    # ========================================================================
    # Serve test (X64 only): driver image runs the HTTP server, the shell
    # returns immediately after `serve`, host curls validate the driver
    # is alive. (No --detach flag -- serve is detach-only as of the
    # foreground removal.)
    # ========================================================================

    info "QEMU" "=== Serve test: X64 ==="

    APP_EFI="$PROJECT_ROOT/build/axl/x64/axl-webfs.efi"
    DETACH_PORT=18091

    if [ ! -f "$APP_EFI" ]; then
        skip "serve: axl-webfs.efi not built"
    else
        DETACH_STAGE=$(mktemp -d)
        echo "detach test" > "$DETACH_STAGE/detach_test.txt"

        DETACH_NSH=$(mktemp --suffix=.nsh)
        cat > "$DETACH_NSH" <<'NSHEOF'
@echo -off
fs0:
axl-webfs.efi serve -p 8080 --log fs0:\webfs.log
echo === DETACHED ===
NSHEOF

        # Embedded-driver model: the serve driver is .incbin'd into
        # axl-webfs.efi via axl-cc --embed, so no separate driver
        # image staging is required.
        eval "$("$RUN_QEMU_SH" --arch X64 --timeout 60 \
            --net --hostfwd "${DETACH_PORT}:8080" \
            --extra "$DETACH_STAGE/detach_test.txt" \
            --nsh "$DETACH_NSH" \
            --background \
            "$APP_EFI")"

        rm -f "$DETACH_NSH"
        rm -rf "$DETACH_STAGE"

        if [ -z "${QEMU_PID:-}" ]; then
            fail "serve: QEMU failed to start"
        else
            READY=false
            for WAIT in $(seq 1 30); do
                sleep 1
                if ! kill -0 "$QEMU_PID" 2>/dev/null; then break; fi
                HTTP_CHECK=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:${DETACH_PORT}/" 2>/dev/null || true)
                if [ "$HTTP_CHECK" = "200" ]; then
                    READY=true; break
                fi
            done

            BASE="http://127.0.0.1:${DETACH_PORT}"

            if $READY; then
                pass "serve: driver came up (HTTP responding)"

                grep -q "DETACHED" "$SERIAL_LOG" 2>/dev/null && \
                    pass "serve: shell returned after detach" || \
                    fail "serve: shell return" "no DETACHED marker in serial log"

                grep -q "listening" "$SERIAL_LOG" 2>/dev/null && \
                    pass "serve: driver banner printed" || \
                    fail "serve: banner" "no listening banner in serial log"

                # Valid --log path success counterpart to serve-stop's
                # bogus-path failure check: a writable fs0:\webfs.log
                # must NOT trip the "cannot open log file" error path.
                if grep -q "ERROR: serve: cannot open log file" \
                       "$SERIAL_LOG" 2>/dev/null; then
                    fail "serve --log: valid path" \
                         "spurious 'cannot open log file' for fs0:\\webfs.log"
                else
                    pass "serve --log: valid fs0:\\webfs.log opens cleanly"
                fi

                CONTENT=$(curl -sf "$BASE/fs0/detach_test.txt" 2>/dev/null)
                echo "$CONTENT" | grep -q "detach test" && \
                    pass "serve: GET works against driver" || \
                    fail "serve: GET" "unexpected: $CONTENT"

                HTTP_CODE=$(echo -n "uploaded" | curl -s -o /dev/null -w "%{http_code}" -T - "$BASE/fs0/detach_upload.txt" 2>/dev/null || true)
                [ "$HTTP_CODE" = "201" ] && \
                    pass "serve: PUT works against driver" || \
                    fail "serve: PUT" "expected 201, got $HTTP_CODE"

                # Multi-chunk PUT: 1 MB exercises ~16 chunk-handler
                # invocations (default upload.chunk.size = 64 KB), so
                # the per-chunk write path is hit repeatedly. Round-
                # trip via GET and compare to verify chunks are
                # ordered correctly. We can't test >128 MB here
                # because run-qemu.sh's auto-sized disk is ~40 MB --
                # multi-GB streaming is verified by SDK-level tests.
                MULTI_CHUNK=$(mktemp)
                dd if=/dev/urandom of="$MULTI_CHUNK" bs=1024 count=1024 \
                    status=none 2>/dev/null
                HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" \
                    -T "$MULTI_CHUNK" \
                    "$BASE/fs0/multichunk.bin" 2>/dev/null || true)
                if [ "$HTTP_CODE" = "201" ]; then
                    pass "serve: multi-chunk streaming PUT (1 MB, ~16 chunks) returned 201"
                    # Full-file GET via the streamer: pulls bytes
                    # chunk-by-chunk from the FtReadCtx, no full-file
                    # malloc.
                    GOT=$(mktemp)
                    curl -sf "$BASE/fs0/multichunk.bin" -o "$GOT" \
                        2>/dev/null || true
                    if cmp -s "$MULTI_CHUNK" "$GOT"; then
                        pass "serve: streaming GET full-file round-trip preserves bytes"
                    else
                        fail "serve: streaming GET round-trip" \
                             "GET'd content differs from PUT"
                    fi
                    rm -f "$GOT"

                    # Range GET via the streamer: opens at offset and
                    # bounds the slice. Pull bytes 100000-101023 (1 KB
                    # slice from inside the 1 MB upload) and verify
                    # status=206 + content matches.
                    SLICE=$(mktemp)
                    HEADERS=$(mktemp)
                    HTTP_CODE=$(curl -s -o "$SLICE" -D "$HEADERS" \
                        -w "%{http_code}" \
                        -H "Range: bytes=100000-101023" \
                        "$BASE/fs0/multichunk.bin" 2>/dev/null || true)
                    if [ "$HTTP_CODE" = "206" ]; then
                        EXPECT=$(mktemp)
                        dd if="$MULTI_CHUNK" of="$EXPECT" bs=1 skip=100000 \
                            count=1024 status=none 2>/dev/null
                        if cmp -s "$EXPECT" "$SLICE"; then
                            pass "serve: streaming Range GET (bytes=100000-101023, 206) matches slice"
                        else
                            fail "serve: streaming Range GET" \
                                 "slice content mismatch"
                        fi
                        rm -f "$EXPECT"

                        # RFC 9110 §15.3.7: 206 responses MUST carry
                        # Content-Range. axl-sdk landed
                        # axl_http_response_set_content_range; the
                        # serve handler invokes it on the Range
                        # branch.
                        if grep -qiE '^Content-Range: bytes 100000-101023/1048576' \
                               "$HEADERS"; then
                            pass "serve: streaming Range GET emits Content-Range"
                        else
                            fail "serve: streaming Range GET" \
                                 "Content-Range header missing or malformed"
                        fi
                    else
                        fail "serve: streaming Range GET" \
                             "expected 206, got $HTTP_CODE"
                    fi
                    rm -f "$SLICE" "$HEADERS"
                else
                    fail "serve: multi-chunk PUT" \
                         "expected 201, got $HTTP_CODE"
                fi
                rm -f "$MULTI_CHUNK"

                # Pubsub-driven console feedback fires on the deferred
                # queue, so we need a beat for the line to land in
                # the serial log after the PUT response is sent.
                sleep 2
                grep -qE "PUT /fs0/detach_upload.txt -> 201" \
                    "$SERIAL_LOG" 2>/dev/null && \
                    pass "serve: console feedback for PUT" || \
                    fail "serve: console feedback" "no PUT line in serial log"

                # ----------------------------------------------------
                # WebDAV class-1 + MOVE smoke (axl_http_server_add_webdav
                # adapter at /dav). The SDK has full unit + integration
                # coverage of the WebDAV protocol layer; here we only
                # verify the axl-webfs adapter wires the FtVolume tree
                # correctly: PROPFIND lists volumes, PUT/GET/MOVE/DELETE
                # round-trip, MKCOL creates a collection. Uses curl with
                # raw WebDAV methods (no cadaver dependency).
                # ----------------------------------------------------

                # PROPFIND on the WebDAV root must enumerate volumes
                # (fs0 at minimum -- run-qemu provides a single FAT
                # volume). The SDK emits 207 Multi-Status XML with one
                # <D:response> per child plus the self entry.
                PROPFIND_OUT=$(curl -s -X PROPFIND -H "Depth: 1" \
                    -w "HTTP:%{http_code}" "$BASE/dav/" 2>/dev/null || true)
                if echo "$PROPFIND_OUT" | grep -q "HTTP:207" && \
                   echo "$PROPFIND_OUT" | grep -q "fs0"; then
                    pass "serve dav: PROPFIND / lists fs0 (207 + fs0 entry)"
                else
                    fail "serve dav: PROPFIND /" \
                         "expected 207 with fs0 entry, got: $PROPFIND_OUT"
                fi

                # PUT via WebDAV (same upload-route streaming path as
                # the REST surface, just under /dav).
                HTTP_CODE=$(echo -n "dav content" | \
                    curl -s -o /dev/null -w "%{http_code}" \
                    -T - "$BASE/dav/fs0/dav-put.txt" 2>/dev/null || true)
                # Built-in SHA-256 integrity on /dav (SDK c14abbc). The
                # adapter's dav_digest callback resolves the path via
                # parse_dav_path and reuses webfs-serve.c's
                # compute_file_digest cache. Expect the response to
                # carry `Digest: sha-256=<64-hex>` when the request
                # opts in with Want-Digest.
                DAV_DIGEST=$(curl -sI -H "Want-Digest: sha-256" \
                    "$BASE/dav/fs0/dav-put.txt" 2>/dev/null \
                    | grep -i "^Digest:" || true)
                if echo "$DAV_DIGEST" | grep -qE "sha-256=[0-9a-f]{64}"; then
                    pass "serve dav: Want-Digest sha-256 → response Digest"
                else
                    fail "serve dav: Want-Digest" \
                         "expected sha-256=<hex>, got: $DAV_DIGEST"
                fi
                [ "$HTTP_CODE" = "201" ] && \
                    pass "serve dav: PUT /dav/fs0/dav-put.txt returns 201" || \
                    fail "serve dav: PUT" "expected 201, got $HTTP_CODE"

                # GET round-trip: bytes survive PROPFIND/MOVE/etc.
                CONTENT=$(curl -sf "$BASE/dav/fs0/dav-put.txt" 2>/dev/null)
                [ "$CONTENT" = "dav content" ] && \
                    pass "serve dav: GET round-trip preserves bytes" || \
                    fail "serve dav: GET" "expected 'dav content', got '$CONTENT'"

                # PUT-side Content-Digest validation (SDK 28d488d).
                # Mismatch → 400 + file is cleaned up (no stale bytes
                # left on the volume). Use a wrong hex; the right one
                # would be sha256("digest-body") =
                # 5e524c54... but we don't care — just need ANY
                # 64-hex that doesn't match.
                HTTP_CODE=$(echo -n "digest-body" | curl -s -o /dev/null \
                    -w "%{http_code}" \
                    -H "Content-Digest: sha-256=0000000000000000000000000000000000000000000000000000000000000000" \
                    -T - "$BASE/dav/fs0/digest-bad.txt" 2>/dev/null || true)
                [ "$HTTP_CODE" = "400" ] && \
                    pass "serve dav: Content-Digest mismatch → 400" || \
                    fail "serve dav: Content-Digest mismatch" \
                         "expected 400, got $HTTP_CODE"

                # The bad PUT should have left no file behind.
                HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" \
                    "$BASE/dav/fs0/digest-bad.txt" 2>/dev/null || true)
                [ "$HTTP_CODE" = "404" ] && \
                    pass "serve dav: rejected PUT leaves no stale file" || \
                    fail "serve dav: rejected PUT cleanup" \
                         "expected 404, got $HTTP_CODE"

                # Matching digest → 201. Pre-compute the SHA-256 of
                # "digest-body" externally to keep the test
                # deterministic.
                DIGEST_OK=$(printf '%s' "digest-body" | sha256sum | cut -d' ' -f1)
                HTTP_CODE=$(echo -n "digest-body" | curl -s -o /dev/null \
                    -w "%{http_code}" \
                    -H "Content-Digest: sha-256=$DIGEST_OK" \
                    -T - "$BASE/dav/fs0/digest-ok.txt" 2>/dev/null || true)
                [ "$HTTP_CODE" = "201" ] && \
                    pass "serve dav: Content-Digest match → 201" || \
                    fail "serve dav: Content-Digest match" \
                         "expected 201, got $HTTP_CODE"

                # MKCOL creates a collection.
                HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" \
                    -X MKCOL "$BASE/dav/fs0/dav-dir" 2>/dev/null || true)
                [ "$HTTP_CODE" = "201" ] && \
                    pass "serve dav: MKCOL /dav/fs0/dav-dir returns 201" || \
                    fail "serve dav: MKCOL" "expected 201, got $HTTP_CODE"

                # MOVE same-directory (rename). axl_file_rename's
                # atomic-on-FAT path inside axl_file_move.
                HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" \
                    -X MOVE -H "Destination: $BASE/dav/fs0/dav-renamed.txt" \
                    "$BASE/dav/fs0/dav-put.txt" 2>/dev/null || true)
                [ "$HTTP_CODE" = "201" ] && \
                    pass "serve dav: MOVE (same-dir rename) returns 201" || \
                    fail "serve dav: MOVE" "expected 201, got $HTTP_CODE"

                # Verify the rename landed: new path serves the
                # original bytes, old path 404s.
                CONTENT=$(curl -sf "$BASE/dav/fs0/dav-renamed.txt" 2>/dev/null)
                [ "$CONTENT" = "dav content" ] && \
                    pass "serve dav: MOVE destination has original bytes" || \
                    fail "serve dav: MOVE destination" "got '$CONTENT'"

                HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" \
                    "$BASE/dav/fs0/dav-put.txt" 2>/dev/null || true)
                [ "$HTTP_CODE" = "404" ] && \
                    pass "serve dav: MOVE source 404s after rename" || \
                    fail "serve dav: MOVE source" "expected 404, got $HTTP_CODE"

                # MOVE cross-directory. axl_file_move falls back to
                # chunked stream copy + source delete when rename
                # refuses the cross-directory request. This is the
                # case the WebDAV adapter couldn't handle before
                # axl-sdk landed axl_file_move.
                HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" \
                    -X MOVE -H "Destination: $BASE/dav/fs0/dav-dir/moved.txt" \
                    "$BASE/dav/fs0/dav-renamed.txt" 2>/dev/null || true)
                [ "$HTTP_CODE" = "201" ] && \
                    pass "serve dav: MOVE (cross-dir, copy+delete) returns 201" || \
                    fail "serve dav: MOVE cross-dir" "expected 201, got $HTTP_CODE"

                CONTENT=$(curl -sf "$BASE/dav/fs0/dav-dir/moved.txt" 2>/dev/null)
                [ "$CONTENT" = "dav content" ] && \
                    pass "serve dav: cross-dir MOVE preserves bytes" || \
                    fail "serve dav: cross-dir MOVE bytes" "got '$CONTENT'"

                # PROPFIND on a real file: getlastmodified should
                # appear now that AxlDirEntry/AxlFileInfo carry
                # mtime_unix.
                PROPFIND_FILE=$(curl -s -X PROPFIND -H "Depth: 0" \
                    "$BASE/dav/fs0/dav-dir/moved.txt" 2>/dev/null || true)
                echo "$PROPFIND_FILE" | grep -q "getlastmodified" && \
                    pass "serve dav: PROPFIND emits getlastmodified" || \
                    fail "serve dav: PROPFIND mtime" \
                         "getlastmodified missing from PROPFIND XML"

                # DELETE the moved file, then the empty collection.
                HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" \
                    -X DELETE "$BASE/dav/fs0/dav-dir/moved.txt" 2>/dev/null \
                    || true)
                { [ "$HTTP_CODE" = "200" ] || [ "$HTTP_CODE" = "204" ]; } && \
                    pass "serve dav: DELETE file returns 2xx" || \
                    fail "serve dav: DELETE file" "expected 2xx, got $HTTP_CODE"

                HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" \
                    -X DELETE "$BASE/dav/fs0/dav-dir" 2>/dev/null || true)
                { [ "$HTTP_CODE" = "200" ] || [ "$HTTP_CODE" = "204" ]; } && \
                    pass "serve dav: DELETE empty collection returns 2xx" || \
                    fail "serve dav: DELETE collection" "expected 2xx, got $HTTP_CODE"
            else
                fail "serve: driver did not start within 30s"
            fi

            kill "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null || true
            rm -rf "$TMPDIR"
        fi
    fi

    # ========================================================================
    # serve-stop verb (X64 only): launch serve, stop, verify idempotent
    # re-stop. All steps run inside the UEFI shell; serial log
    # assertions verify each phase.
    # ========================================================================

    info "QEMU" "=== Serve-stop test: X64 ==="

    APP_EFI="$PROJECT_ROOT/build/axl/x64/axl-webfs.efi"

    if [ ! -f "$APP_EFI" ]; then
        skip "serve-stop: axl-webfs.efi not built"
    else
        STOP_NSH=$(mktemp --suffix=.nsh)
        cat > "$STOP_NSH" <<'NSHEOF'
@echo -off
fs0:
axl-webfs.efi serve -p 8080 --log fs99:\nope.log
echo === DETACHED ===
axl-webfs.efi serve-stop
echo === STOPPED ===
axl-webfs.efi serve-stop
echo === RE-STOP ===
NSHEOF

        eval "$("$RUN_QEMU_SH" --arch X64 --timeout 60 \
            --net \
            --nsh "$STOP_NSH" \
            --background \
            "$APP_EFI")"

        rm -f "$STOP_NSH"

        if [ -z "${QEMU_PID:-}" ]; then
            fail "serve-stop: QEMU failed to start"
        else
            # Wait for the nsh to finish all three markers (or timeout).
            for WAIT in $(seq 1 30); do
                sleep 1
                if grep -q "RE-STOP" "$SERIAL_LOG" 2>/dev/null; then
                    break
                fi
                kill -0 "$QEMU_PID" 2>/dev/null || break
            done

            grep -q "listening" "$SERIAL_LOG" 2>/dev/null && \
                pass "serve-stop: detach launched the driver" || \
                fail "serve-stop: detach" "no listening banner"

            # The bogus --log path (fs99:\nope.log) should fail to open
            # but the service must still start and the failure must be
            # surfaced clearly to the console.
            grep -qE "ERROR: serve: cannot open log file 'fs99:" \
                "$SERIAL_LOG" 2>/dev/null && \
                pass "serve --log: bogus path surfaces console error" || \
                fail "serve --log: bogus path" "no console error for unopenable log"

            grep -q "axl-webfs: stopping serve\.\.\." "$SERIAL_LOG" 2>/dev/null && \
            grep -q "axl-webfs: serve stopped" "$SERIAL_LOG" 2>/dev/null && \
                pass "serve-stop: first stop unloaded the driver" || \
                fail "serve-stop: first stop" "no stopping/stopped lines"

            grep -q "axl-webfs: no serve running" "$SERIAL_LOG" 2>/dev/null && \
                pass "serve-stop: re-stop is idempotent (exit 0, no error)" || \
                fail "serve-stop: idempotent re-stop" "second stop did not report 'no serve running'"

            grep -q "RE-STOP" "$SERIAL_LOG" 2>/dev/null && \
                pass "serve-stop: shell completed all three commands" || \
                fail "serve-stop: shell completion" "RE-STOP marker missing"

            kill "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null || true
            rm -rf "$TMPDIR"
        fi
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
