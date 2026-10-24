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

        # The `ls fs1:\` output only, and readme.txt's WHOLE listing row:
        # the name alone also appears in the script's own
        # `type fs1:\readme.txt` and in any error that names it, so a bare
        # grep passes on an empty listing. The row is deterministic: the
        # mount reports no modification time (00/00/0000 00:00) and the
        # file is "mount test content\n", 19 bytes.
        LS_ROOT=$(sed -n '/=== TEST: ls mounted volume ===/,/=== TEST: read file ===/p' \
            "$SERIAL_LOG" 2>/dev/null | tr -d '\r')
        LS_ROW=$(printf '00/00/0000  00:00%20s  readme.txt' 19)
        echo "$LS_ROOT" | grep -qxF "$LS_ROW" && \
            pass "$QEMU_ARCH: ls shows remote files (readme.txt)" || \
            fail "$QEMU_ARCH: ls" "no readme.txt row in: ${LS_ROOT:0:300}"

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

        # Non-ASCII filename round-trip — 'résumé.txt' and
        # '日本語.bin' must survive the JSON listing, the
        # AxlFsProvider name marshalling and the EFI_FILE_INFO
        # trailer into Shell `ls` output, byte-exact.
        #
        # How a non-ASCII CHAR16 cell REACHES the serial log is
        # firmware-specific, so accept either known rendering:
        #   - OVMF / X64 encodes the cell as UTF-8, so the real
        #     name appears verbatim ('résumé.txt'). Verified: a
        #     Shell error naming cells U+00C3 U+00A9 came back as
        #     0xC3 0x83 0xC2 0xA9, the UTF-8 of both.
        #   - ArmVirtQemu / AARCH64 prints each cell's LOW BYTE,
        #     substituting '?' when it is not printable — so 'é'
        #     (U+00E9) becomes '?' and '本' (U+672C, low byte 0x2C)
        #     becomes ','. Hence 'r?sum?.txt' and '?,?.bin'.
        #
        # Either way the CELL COUNT is what carries the signal, and
        # it still discriminates: a Latin-1 cast doubles the cells
        # ('r??sum??.txt'), which the negative check below rejects.
        #
        # Two distinct corruptions are guarded explicitly, because
        # both once passed a weaker version of this test:
        #
        #  1. Latin-1 cast of the UTF-8 bytes — 0xC3 0xA9 widened to
        #     cells U+00C3 U+00A9, which reaches the log as 'Ã©'.
        #  2. Undecoded JSON \uXXXX. xfer-server.py is Python
        #     json.dumps, which defaults to ensure_ascii=True and so
        #     sends the ESCAPED form on the wire — the 20-byte
        #     ASCII "r\u00e9sum\u00e9.txt", not the 12-byte UTF-8
        #     name. An SDK whose JSON string
        #     accessors do not decode \uXXXX yields the LITERAL
        #     ASCII text 'ru00e9sumu00e9.txt' (axl-sdk 0352d185).
        #     That is what this test asserted until 2026-08-04 — it
        #     was pinning the corruption, having misread the literal
        #     text as an OVMF unprintable-cell fallback rendering.
        if grep -qE 'résumé\.txt|r\?sum\?\.txt' "$SERIAL_LOG" 2>/dev/null; then
            pass "$QEMU_ARCH: non-ASCII 'résumé.txt' (U+00E9) round-trip"
        else
            fail "$QEMU_ARCH: non-ASCII résumé.txt" \
                 "expected 'résumé.txt' or 'r?sum?.txt' in ls output"
        fi
        if grep -qE '日本語\.bin|\?,\?\.bin' "$SERIAL_LOG" 2>/dev/null; then
            pass "$QEMU_ARCH: non-ASCII '日本語.bin' (3-byte UTF-8) round-trip"
        else
            fail "$QEMU_ARCH: non-ASCII 日本語.bin" \
                 "expected '日本語.bin' or '?,?.bin' in ls output"
        fi
        if grep -qE 'Ã©|r\?\?sum\?\?\.txt' "$SERIAL_LOG" 2>/dev/null; then
            fail "$QEMU_ARCH: UTF-8 Latin-1 cast regression" \
                 "saw doubled cells — axl_utf8_to_ucs2_buf reverted to byte cast?"
        else
            pass "$QEMU_ARCH: no Latin-1-cast smearing"
        fi
        if grep -q 'ru00e9sumu00e9' "$SERIAL_LOG" 2>/dev/null; then
            fail "$QEMU_ARCH: undecoded JSON \\uXXXX" \
                 "saw literal 'ru00e9sumu00e9' — JSON string accessors stopped decoding \\uXXXX?"
        else
            pass "$QEMU_ARCH: JSON \\uXXXX decoded (no literal 'ru00e9sumu00e9')"
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
        mkdir -p "$DAV_TEST_DIR/sp ace"
        echo "rename me" > "$DAV_TEST_DIR/sp ace/a.txt"

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
            rm -f "$DAV_TEST_DIR/sp ace/b.txt"
            echo "rename me" > "$DAV_TEST_DIR/sp ace/a.txt"

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
echo === TEST: rename ===
mv "fs1:\\sp ace\\a.txt" "fs1:\\sp ace\\b.txt"
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

            # A rename over WebDAV to a real server: its MOVE carries a
            # Destination with our host:port, which the server checks
            # against the request's Host -- so Host must carry the port
            # too (a mismatch is 502 Bad Gateway).
            [ -f "$DAV_TEST_DIR/sp ace/b.txt" ] && [ ! -e "$DAV_TEST_DIR/sp ace/a.txt" ] && \
                pass "$QEMU_ARCH dav: Shell mv renames on the server" || \
                fail "$QEMU_ARCH dav: rename" "expected 'sp ace/b.txt' and no 'sp ace/a.txt' on the server"

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
    # Synthetic trees (scripts/listing-server.py): listings no real host
    # directory can stand in for. listing_mount CASE PROTO CMD... boots one
    # mount of the fixture, runs the shell CMDs between "=== OUT ===" and
    # "=== OUT DONE ===", and leaves that span, with line breaks removed
    # (a long name can wrap on the console), in $LM_OUT and the whole log
    # in $LM_LOG. LM_FIXTURE_ARGS (an array) adds fixture options and
    # LM_URL_PATH replaces the mount URL's path ("/" by default).
    # lm_rows prints the names of the listing rows in $LM_LOG between two
    # markers, sorted, one per line.
    # ========================================================================

    LM_PORT=$((SERVER_PORT + 3))
    listing_mount() {
        local lm_case="$1" lm_proto="$2"
        shift 2
        local lm_flag=""
        [ "$lm_proto" = "dav" ] && lm_flag="--webdav"
        python3 "$SCRIPT_DIR/listing-server.py" --case "$lm_case" \
            --port "$LM_PORT" --bind 0.0.0.0 $lm_flag "${LM_FIXTURE_ARGS[@]}" \
            >/tmp/listing-server.log 2>&1 &
        local lm_pid=$!
        sleep 0.5
        local lm_nsh
        lm_nsh=$(mktemp --suffix=.nsh)
        {
            echo "@echo -off"
            echo "fs0:"
            echo "axl-webfs.efi mount --protocol $lm_proto \"http://10.0.2.2:${LM_PORT}${LM_URL_PATH:-/}\""
            echo "map -r"
            echo "echo === OUT ==="
            printf '%s\n' "$@"
            echo "echo === OUT DONE ==="
            echo "axl-webfs.efi umount"
            echo "reset -s"
        } > "$lm_nsh"
        LM_LOG=$(mktemp)
        "$RUN_QEMU_SH" --arch "$LM_ARCH" --timeout 40 --raw --net \
            --nsh "$lm_nsh" --serial-log "$LM_LOG" \
            "$LM_EFI" > /dev/null 2>&1 || true
        rm -f "$lm_nsh"
        kill "$lm_pid" 2>/dev/null || true
        wait "$lm_pid" 2>/dev/null || true
        LM_OUT=$(sed -n '/=== OUT ===/,/=== OUT DONE ===/p' "$LM_LOG" \
            2>/dev/null | tr -d '\r\n')
    }
    LM_FIXTURE_ARGS=()
    lm_rows() {
        sed -n "/$1/,/$2/p" "$LM_LOG" | tr -d '\r' | \
            grep -E '^[0-9/]+  [0-9:]+ ' | \
            sed -E 's/^[0-9/]+  [0-9:]+ (<DIR>)? *[0-9]+  //' | LC_ALL=C sort -f
    }

    for LM_ARCH in "${QEMU_ARCHS[@]}"; do
        LM_DIR=$(echo "$LM_ARCH" | tr '[:upper:]' '[:lower:]')
        [ "$LM_DIR" = "aarch64" ] && LM_DIR="aa64"
        LM_EFI="$PROJECT_ROOT/build/axl/$LM_DIR/axl-webfs.efi"
        if [ ! -f "$LM_EFI" ]; then continue; fi

        # Over-long names (WEBFS_NAME_SIZE): a name that does not fit the
        # mount's 256-byte name buffers is SKIPPED, never cut -- a cut
        # name can equal a sibling's.
        for LM_PROTO in json dav; do
            T="$LM_ARCH long names ($LM_PROTO)"
            info "QEMU" "$T: Booting mount test..."
            listing_mount long-names "$LM_PROTO" 'ls fs1:\'

            # Control: the listing was fetched and shown at all.
            echo "$LM_OUT" | grep -q "readme.txt" && \
                pass "$T: listing shows readme.txt" || \
                fail "$T: listing" "readme.txt not in the ls output"

            # The 255-byte name is listed once, whole, with ITS size (5):
            # never the cut 256-byte one (size 6) in its place or beside it.
            LN_SEEN=$(echo "$LM_OUT" | grep -oE "[0-9]+  LONGNAME-a*" | \
                sed -E 's/^([0-9]+)  (.*)$/\1 \2/' | \
                awk '{print $1 ":" length($2)}' | tr '\n' ' ')
            [ "$LN_SEEN" = "5:255 " ] && \
                pass "$T: the 255-byte name is listed once, whole" || \
                fail "$T: 255-byte name" \
                     "expected '5:255 ' (size:name-length), got '$LN_SEEN'"

            grep -qa "skipping an entry whose name" "$LM_LOG" && \
                pass "$T: the 256-byte name is skipped, not cut" || \
                fail "$T: 256-byte name" "no 'skipping an entry' warning"

            if [ "$LM_PROTO" = "dav" ]; then
                LN_HREF_SEEN=$(echo "$LM_OUT" | grep -oE 'HREFNAME-b*' | \
                    awk '{print length}' | tr '\n' ' ')
                [ "$LN_HREF_SEEN" = "255 " ] && \
                    pass "$T: a 765-character href yields its whole 255-byte name" || \
                    fail "$T: long href" \
                         "expected one 255-byte HREFNAME, got lengths '$LN_HREF_SEEN'"
            fi
            rm -f "$LM_LOG"
        done

        # WebDAV displaynames are XML text: "R&amp;D &lt;notes&gt;.txt"
        # names "R&D <notes>.txt", and "part1<![CDATA[&part2]]>.txt" --
        # three text tokens -- names "part1&part2.txt".
        T="$LM_ARCH dav displayname"
        info "QEMU" "$T: Booting mount test..."
        listing_mount entities dav 'ls fs1:\'
        echo "$LM_OUT" | grep -qF "5  R&D <notes>.txt" && \
            pass "$T: &amp; / &lt; / &gt; decode to 'R&D <notes>.txt'" || \
            fail "$T: entities" "no 'R&D <notes>.txt' row in: ${LM_OUT:0:300}"
        echo "$LM_OUT" | grep -qF "5  part1&part2.txt" && \
            pass "$T: a displayname split by CDATA is joined whole" || \
            fail "$T: CDATA" "no 'part1&part2.txt' row in: ${LM_OUT:0:300}"
        rm -f "$LM_LOG"

        # A name is encoded WHOLE in a request: a '?' is a name byte
        # (%3F), not the start of a query the server splits off; and a
        # directory's own name is not percent-decoded again when its
        # PROPFIND self entry is matched -- "pct%41dir" would read as
        # "pctAdir", and the directory list itself as its own child.
        for LM_PROTO in json dav; do
            T="$LM_ARCH name encoding ($LM_PROTO)"
            info "QEMU" "$T: Booting mount test..."
            listing_mount entities "$LM_PROTO" \
                "echo === Q ===" 'type fs1:\what?.txt' "echo === R ===" 'ls -r fs1:\'
            Q_OUT=$(echo "$LM_OUT" | sed -n 's/.*=== Q ===\(.*\)=== R ===.*/\1/p')
            [ "$Q_OUT" = "question" ] && \
                pass "$T: a name with '?' is read whole" || \
                fail "$T: '?' in a name" "expected 'question', got '${Q_OUT:0:120}'"
            PCT_BLOCK=$(sed -n '/=== R ===/,/=== OUT DONE ===/p' "$LM_LOG" | tr -d '\r' | \
                sed -n '/^Directory of: FS1:\\pct%41dir\\$/,/Dir(s)$/p' | \
                grep -E "File\(s\)|Dir\(s\)" | tr -s ' ' | tr '\n' '|')
            [ "$PCT_BLOCK" = " 1 File(s) 5 bytes| 0 Dir(s)|" ] && \
                pass "$T: a directory named 'pct%41dir' lists only its own child" || \
                fail "$T: '%' in a directory name" "expected ' 1 File(s) 5 bytes| 0 Dir(s)|', got '$PCT_BLOCK'"
            rm -f "$LM_LOG"
        done

        # A rename on the mount reaches the server (the fs-provider hands
        # Shell `mv` to the mount since its version 2), and its WebDAV
        # Destination names the directory encoded whole: a directory
        # with a space must arrive as that name. The fixture logs the
        # decoded path of each MOVE's Destination.
        T="$LM_ARCH rename (dav)"
        info "QEMU" "$T: Booting mount test..."
        listing_mount entities dav \
            'mv "fs1:\sp ace\a.txt" "fs1:\sp ace\b.txt"' "echo === LS ===" 'ls "fs1:\sp ace\"'
        MV_DEST=$(grep -a "MOVE-DEST" /tmp/listing-server.log | sed 's/^ *MOVE-DEST //')
        MV_LS=$(echo "$LM_OUT" | sed -n 's/.*=== LS ===\(.*\)=== OUT DONE ===.*/\1/p')
        [ "$MV_DEST" = "/sp ace/b.txt" ] && echo "$MV_LS" | grep -q "  b\.txt" && \
            ! echo "$MV_LS" | grep -q "  a\.txt" && \
            pass "$T: Shell mv renames over WebDAV, Destination '/sp ace/b.txt'" || \
            fail "$T: mv" "expected Destination '/sp ace/b.txt' and b.txt listed, got '$MV_DEST' and: ${MV_LS:0:200}"
        rm -f "$LM_LOG"

        # A share below the server's root: the mount URL's path selects it
        # ("/my share/", a base that needs encoding), and every href is
        # normalized and read relative to it. Of the fixture's root hrefs
        # these are entries: dot.txt (a "." segment), readme.txt, rel.txt
        # (relative, resolved against the request), upper.txt (our host
        # and port under an upper-case "HTTP://"), netours.txt (a
        # "//host:port" reference to us), qf.txt (its "?x=1#frag" is no
        # part of the name), real.txt and dn.txt (named by their hrefs,
        # not their displaynames "fake.txt" and "../other/evil") and
        # "sp ace". Not entries: the self entry, an href outside the share,
        # one climbing out of it with "..", one that is no child of the
        # root, and every href for another host or port -- absolute or
        # "//other-host/...". A rename under the base carries the base in
        # its Destination.
        T="$LM_ARCH share base (dav)"
        info "QEMU" "$T: Booting mount test..."
        LM_FIXTURE_ARGS=(--base "/my share")
        LM_URL_PATH="/my share/"   # a script cannot hand the Shell "%20"
        listing_mount based dav \
            "echo === ROOT ===" 'ls fs1:\' "echo === MV ===" \
            'mv "fs1:\sp ace\a.txt" "fs1:\sp ace\b.txt"' \
            "echo === SUB ===" 'ls "fs1:\sp ace\"' "echo === END ==="
        LM_FIXTURE_ARGS=()
        unset LM_URL_PATH
        BASE_ROOT=$(lm_rows "=== ROOT ===" "=== MV ===" | tr '\n' '|')
        BASE_WANT="dn.txt|dot.txt|netours.txt|qf.txt|readme.txt|real.txt|rel.txt|sp ace|upper.txt|"
        [ "$BASE_ROOT" = "$BASE_WANT" ] && \
            pass "$T: the root lists exactly its nine entries" || \
            fail "$T: root" "expected '$BASE_WANT', got '$BASE_ROOT'"
        MV_DEST=$(grep -a "MOVE-DEST" /tmp/listing-server.log | sed 's/^ *MOVE-DEST //')
        BASE_SUB=$(lm_rows "=== SUB ===" "=== END ===" | tr '\n' '|')
        [ "$MV_DEST" = "/my share/sp ace/b.txt" ] && [ "$BASE_SUB" = "b.txt|" ] && \
            pass "$T: a rename under the base lands, Destination '/my share/sp ace/b.txt'" || \
            fail "$T: rename" "expected '/my share/sp ace/b.txt' and 'b.txt|', got '$MV_DEST' and '$BASE_SUB'"
        rm -f "$LM_LOG"

        # A listing that cannot be represented whole is refused, never
        # shown empty or short: a PROPFIND with no entry for the listed
        # collection itself (the share the hrefs should sit under is not
        # where the server lists) fails the listing loudly.
        T="$LM_ARCH share base, no self entry (dav)"
        info "QEMU" "$T: Booting mount test..."
        LM_FIXTURE_ARGS=(--base "/my share" --no-self)
        LM_URL_PATH="/my share/"   # a script cannot hand the Shell "%20"
        listing_mount based dav "echo === ROOT ===" 'ls fs1:\' "echo === END ==="
        LM_FIXTURE_ARGS=()
        unset LM_URL_PATH
        NOSELF_ROWS=$(lm_rows "=== ROOT ===" "=== END ===" | wc -l)
        if grep -qa "has no entry for the listed collection itself; refused" "$LM_LOG" && \
           [ "$NOSELF_ROWS" = "0" ]; then
            pass "$T: a listing without its self entry is refused, not shown"
        else
            fail "$T: no self entry" "expected the refusal line and no rows, got $NOSELF_ROWS rows"
        fi
        rm -f "$LM_LOG"

        # An href that cannot be read fails the whole listing -- never a
        # short one: a %00 in it (it would decode to a name cut at the
        # NUL), and an absolute href with no host.
        for BAD_HREF in '{B}/a%00evil.txt' 'http:///x.txt'; do
            T="$LM_ARCH unreadable href (dav) $BAD_HREF"
            info "QEMU" "$T: Booting mount test..."
            LM_FIXTURE_ARGS=(--base "/my share" --extra-href "$BAD_HREF")
            LM_URL_PATH="/my share/"
            listing_mount based dav "echo === ROOT ===" 'ls fs1:\' "echo === END ==="
            LM_FIXTURE_ARGS=()
            unset LM_URL_PATH
            BADH_ROWS=$(lm_rows "=== ROOT ===" "=== END ===" | wc -l)
            if grep -qa "an href cannot be read; refused" "$LM_LOG" && \
               [ "$BADH_ROWS" = "0" ]; then
                pass "$T: the listing is refused, no rows shown"
            else
                fail "$T: unreadable href" "expected the refusal line and no rows, got $BADH_ROWS rows"
            fi
            rm -f "$LM_LOG"
        done

        # A share path that names a FILE is no share: the probe's 207 for
        # it carries no <collection/>, and the mount is refused.
        T="$LM_ARCH share names a file (dav)"
        info "QEMU" "$T: Booting mount test..."
        LM_FIXTURE_ARGS=(--base "/my share")
        LM_URL_PATH="/my share/readme.txt"
        listing_mount based dav "echo === ROOT ===" 'ls fs1:\' "echo === END ==="
        LM_FIXTURE_ARGS=()
        unset LM_URL_PATH
        if grep -qa "axl-webfs-mount: the share is no WebDAV collection on the server" "$LM_LOG" && \
           ! grep -qa "axl-webfs-mount: mounted" "$LM_LOG"; then
            pass "$T: a share path naming a file is not mounted"
        else
            fail "$T: file as share" "expected the refusal line and no mount"
        fi
        rm -f "$LM_LOG"

        # A share that does not exist on the server is no mount: the probe
        # asks for its root collection, and a 404 refuses the mount rather
        # than show an empty volume.
        T="$LM_ARCH missing share (dav)"
        info "QEMU" "$T: Booting mount test..."
        LM_FIXTURE_ARGS=(--base "/my share")
        LM_URL_PATH="/no such share/"
        listing_mount based dav "echo === ROOT ===" 'ls fs1:\' "echo === END ==="
        LM_FIXTURE_ARGS=()
        unset LM_URL_PATH
        if grep -qa "axl-webfs-mount: the share is no WebDAV collection on the server" "$LM_LOG" && \
           ! grep -qa "axl-webfs-mount: mounted" "$LM_LOG"; then
            pass "$T: a share the server does not have is not mounted"
        else
            fail "$T: missing share" "expected the refusal line and no mount"
        fi
        rm -f "$LM_LOG"

        # A share path the server would normalize to another place -- a
        # ".", ".." or empty segment, or an encoded separator -- is refused
        # at mount time, with a line on the console, never mounted.
        T="$LM_ARCH share path refused"
        info "QEMU" "$T: Booting mount test..."
        BADP_NSH=$(mktemp --suffix=.nsh)
        BADP_LOG=$(mktemp)
        {
            echo "@echo -off"
            echo "fs0:"
            # (An encoded separator, %2F, is refused the same way, but no
            # Shell script can pass "%2F": it expands "%2" as its second
            # argument and fails a "^%" escape. Not tested here.)
            for BADP in "/a/../b/" "/a/./b/" "/a//b/"; do
                echo "axl-webfs.efi mount --protocol dav http://10.0.2.2:${LM_PORT}${BADP}"
            done
            echo "reset -s"
        } > "$BADP_NSH"
        "$RUN_QEMU_SH" --arch "$LM_ARCH" --timeout 40 --raw --net \
            --nsh "$BADP_NSH" --serial-log "$BADP_LOG" "$LM_EFI" > /dev/null 2>&1 || true
        BADP_REFUSED=$(grep -ac "axl-webfs-mount: refused: the share path" "$BADP_LOG" || true)
        if [ "$BADP_REFUSED" = "3" ] && ! grep -qa "axl-webfs-mount: mounted" "$BADP_LOG"; then
            pass "$T: '.', '..' and '//' in the share path are refused"
        else
            fail "$T: share path" "expected 3 refusals and no mount, got $BADP_REFUSED"
        fi
        rm -f "$BADP_NSH" "$BADP_LOG"

        # A big directory lists completely: 151 files, a JSON listing of
        # ~19 KB and a PROPFIND of ~50 KB, past both a 4 KB body and a
        # 64-entry table.
        for LM_PROTO in json dav; do
            T="$LM_ARCH big listing ($LM_PROTO)"
            info "QEMU" "$T: Booting mount test..."
            listing_mount big "$LM_PROTO" 'ls fs1:\'
            BIG_FILES=$(echo "$LM_OUT" | grep -oE "[0-9]+ File\(s\)" || true)
            [ "$BIG_FILES" = "151 File(s)" ] && \
                pass "$T: all 151 files listed" || \
                fail "$T: count" "expected '151 File(s)', got '$BIG_FILES'"
            BIG_LAST="big-149-$(printf 'p%.0s' $(seq 1 40)).txt"
            echo "$LM_OUT" | grep -qF "$BIG_LAST" && \
                pass "$T: the last entry is listed" || \
                fail "$T: last entry" "$BIG_LAST not listed"
            rm -f "$LM_LOG"
        done

        # The JSON listing is a network body: parsed as RFC 8259, so a
        # listing with a trailing comma (JSON5, not JSON) is refused, not
        # read leniently.
        T="$LM_ARCH strict json"
        info "QEMU" "$T: Booting mount test..."
        listing_mount relaxed json 'ls fs1:\'
        if grep -qa "is not valid RFC 8259; refused" "$LM_LOG" && \
           ! echo "$LM_OUT" | grep -q "readme.txt"; then
            pass "$T: a listing with a trailing comma is refused"
        else
            fail "$T: trailing comma" "expected the refusal line and no rows"
        fi
        rm -f "$LM_LOG"

        # A listing past WEBFS_LIST_MAX_ENTRIES (16384) is REFUSED, loudly
        # -- an error to the Shell and a log line -- never cut to the
        # first 16384.
        for LM_PROTO in json dav; do
            T="$LM_ARCH listing cap ($LM_PROTO)"
            info "QEMU" "$T: Booting mount test..."
            listing_mount huge "$LM_PROTO" 'ls fs1:\'
            HUGE_ROWS=$(echo "$LM_OUT" | grep -oE "h-[0-9]{5}\.txt" | wc -l)
            if grep -qa "has more than 16384 entries; refused" "$LM_LOG" && \
               [ "$HUGE_ROWS" = "0" ]; then
                pass "$T: 16385 entries are refused, not listed or cut"
            else
                fail "$T: 16385 entries" \
                     "expected the refusal line and no rows, got $HUGE_ROWS rows"
            fi
            rm -f "$LM_LOG"
        done

        # A request path is sent whole, never cut. "/files" + the 505-byte
        # path is a 511-byte request target and "/files" + its 511-byte
        # sibling a 517-byte one, which starts with the first: a client
        # that cut targets at 511 bytes would read "short-file" for the
        # sibling. Both are read, each as itself; the mount's own limit
        # is its 511-byte path (MAX_PATH_LEN), not the request's size.
        T="$LM_ARCH long path"
        info "QEMU" "$T: Booting mount test..."
        DEEP_DIR="$(printf 'd%.0s' $(seq 1 250))\\$(printf 'e%.0s' $(seq 1 250))"
        listing_mount deep json \
            "echo === SHORT ===" "type fs1:\\$DEEP_DIR\\ff" \
            "echo === LONG ===" "type fs1:\\$DEEP_DIR\\ffgggggg"
        DEEP_SHORT_OUT=$(echo "$LM_OUT" | sed -n 's/.*=== SHORT ===\(.*\)=== LONG ===.*/\1/p')
        DEEP_LONG_OUT=$(echo "$LM_OUT" | sed -n 's/.*=== LONG ===\(.*\)=== OUT DONE ===.*/\1/p')
        echo "$DEEP_SHORT_OUT" | grep -q "short-file" && \
            pass "$T: a 505-byte path is read" || \
            fail "$T: 505-byte path" "no 'short-file' in: ${DEEP_SHORT_OUT:0:200}"
        if echo "$DEEP_LONG_OUT" | grep -q "short-file"; then
            fail "$T: 511-byte path" "read the 505-byte sibling's content"
        elif echo "$DEEP_LONG_OUT" | grep -q "long-file"; then
            pass "$T: a 511-byte path (a 517-byte request) is read whole"
        else
            fail "$T: 511-byte path" "no 'long-file' in: ${DEEP_LONG_OUT:0:200}"
        fi
        rm -f "$LM_LOG"

        # The same tree over WebDAV, where a request carries no "/files"
        # prefix: a 511-byte path is read whole. A 520-byte one -- whose
        # 511-byte cut IS that file -- must never be read as it. Nothing
        # on the way cuts it now: the SDK's fs-provider refuses an EFI
        # name of 512 or more characters instead of cutting it (the UEFI
        # Shell may refuse it first; from outside the two look the same),
        # and the mount refuses a path of 512 bytes or more. What this
        # checks is the outcome: "Cannot open file" from the Shell, and
        # no request for the cut at the server.
        T="$LM_ARCH long path (dav)"
        info "QEMU" "$T: Booting mount test..."
        listing_mount deep dav \
            "echo === SHORT ===" "type fs1:\\$DEEP_DIR\\ffgggggg" \
            "echo === LONG ===" "type fs1:\\$DEEP_DIR\\ffggggggggggggggg"
        DEEP_SHORT_OUT=$(echo "$LM_OUT" | sed -n 's/.*=== SHORT ===\(.*\)=== LONG ===.*/\1/p')
        DEEP_LONG_OUT=$(echo "$LM_OUT" | sed -n 's/.*=== LONG ===\(.*\)=== OUT DONE ===.*/\1/p')
        echo "$DEEP_SHORT_OUT" | grep -q "long-file" && \
            pass "$T: a 511-byte path is read" || \
            fail "$T: 511-byte path" "no 'long-file' in: ${DEEP_SHORT_OUT:0:200}"
        if echo "$DEEP_LONG_OUT" | grep -q "long-file"; then
            fail "$T: 520-byte path" "read as its 511-byte cut"
        elif echo "$DEEP_LONG_OUT" | grep -q "over-file"; then
            fail "$T: 520-byte path" "read whole past the mount's own limit"
        elif echo "$DEEP_LONG_OUT" | grep -q "Cannot open file" && \
             [ "$(grep -c '/ffgggggg HTTP' /tmp/listing-server.log)" = "1" ]; then
            pass "$T: a 520-byte path is refused before any request, never read as its cut"
        else
            fail "$T: 520-byte path" \
                 "expected the Shell's 'Cannot open file' and no request for the cut, got: ${DEEP_LONG_OUT:0:200}"
        fi
        rm -f "$LM_LOG"
    done

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
            kill "$QEMU_PID" 2>/dev/null || true; wait "$QEMU_PID" 2>/dev/null || true
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

        kill "$QEMU_PID" 2>/dev/null || true; wait "$QEMU_PID" 2>/dev/null || true
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

        kill "$QEMU_PID" 2>/dev/null || true; wait "$QEMU_PID" 2>/dev/null || true
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

        kill "$QEMU_PID" 2>/dev/null || true; wait "$QEMU_PID" 2>/dev/null || true
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

        kill "$QEMU_PID" 2>/dev/null || true; wait "$QEMU_PID" 2>/dev/null || true
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

        kill "$QEMU_PID" 2>/dev/null || true; wait "$QEMU_PID" 2>/dev/null || true
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
axl-webfs.efi serve -p 8080 -v --log fs0:\webfs.log
echo === DETACHED ===
NSHEOF

        # Embedded-driver model: the serve driver is .incbin'd into
        # axl-webfs.efi via axl-cc --embed, so no separate driver
        # image staging is required. The guest must outlive every check
        # this block runs (~2 minutes of PUTs, hang-ups and listings),
        # or the later ones see a dead server.
        eval "$("$RUN_QEMU_SH" --arch X64 --timeout 300 \
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

                # ----------------------------------------------------
                # Atomic PUT. The SDK ends every write with
                # write_close(aborted): true DISCARDS the body (client
                # hang-up, Content-Digest mismatch, failed chunk). A
                # discarded PUT must leave the old file exactly as it
                # was, so the body goes to a temp sibling and only a
                # commit replaces the target.
                # ----------------------------------------------------
                ATOMIC_URL="$BASE/dav/fs0/atomic.txt"
                HTTP_CODE=$(printf '%s' "the original, longer contents" | \
                    curl -s -o /dev/null -w "%{http_code}" -T - \
                    "$ATOMIC_URL" 2>/dev/null || true)
                [ "$HTTP_CODE" = "201" ] && \
                    pass "serve dav atomic: first PUT creates the file (201)" || \
                    fail "serve dav atomic: first PUT" "expected 201, got $HTTP_CODE"

                # A replace by a SHORTER body leaves no stale tail.
                HTTP_CODE=$(printf '%s' "short-v1" | \
                    curl -s -o /dev/null -w "%{http_code}" -T - \
                    "$ATOMIC_URL" 2>/dev/null || true)
                CONTENT=$(curl -sf "$ATOMIC_URL" 2>/dev/null || true)
                [ "$HTTP_CODE" = "201" ] && [ "$CONTENT" = "short-v1" ] && \
                    pass "serve dav atomic: replacing PUT stores exactly the new body" || \
                    fail "serve dav atomic: replacing PUT" \
                         "expected 201 + 'short-v1', got $HTTP_CODE + '${CONTENT:0:60}'"

                # A same-SIZE replace: the bytes AND the advertised
                # digest must both be the new body's. The digest cache
                # is keyed by (path, size), so a commit that does not
                # invalidate it serves the old body's digest.
                HTTP_CODE=$(printf '%s' "SHORT-V2" | \
                    curl -s -o /dev/null -w "%{http_code}" -T - \
                    "$ATOMIC_URL" 2>/dev/null || true)
                CONTENT=$(curl -sf "$ATOMIC_URL" 2>/dev/null || true)
                [ "$HTTP_CODE" = "201" ] && [ "$CONTENT" = "SHORT-V2" ] && \
                    pass "serve dav atomic: same-size replacing PUT stores the new bytes" || \
                    fail "serve dav atomic: same-size replace" \
                         "expected 201 + 'SHORT-V2', got $HTTP_CODE + '${CONTENT:0:60}'"
                WANT_HEX=$(printf '%s' "SHORT-V2" | sha256sum | cut -d' ' -f1)
                GOT_DIGEST=$(curl -sI -H "Want-Digest: sha-256" "$ATOMIC_URL" \
                    2>/dev/null | tr -d '\r' | grep -i '^Digest:' || true)
                [ "$GOT_DIGEST" = "Digest: sha-256=$WANT_HEX" ] && \
                    pass "serve dav atomic: Digest after a same-size replace is the new body's" || \
                    fail "serve dav atomic: Digest after replace" \
                         "expected 'Digest: sha-256=$WANT_HEX', got '$GOT_DIGEST'"

                # Content-Digest mismatch: 400, and the OLD file stays.
                HTTP_CODE=$(printf '%s' "a body that must not land" | \
                    curl -s -o /dev/null -w "%{http_code}" \
                    -H "Content-Digest: sha-256=0000000000000000000000000000000000000000000000000000000000000000" \
                    -T - "$ATOMIC_URL" 2>/dev/null || true)
                CONTENT=$(curl -sf "$ATOMIC_URL" 2>/dev/null || true)
                [ "$HTTP_CODE" = "400" ] && [ "$CONTENT" = "SHORT-V2" ] && \
                    pass "serve dav atomic: digest-mismatch PUT (400) keeps the old file" || \
                    fail "serve dav atomic: digest mismatch" \
                         "expected 400 + 'SHORT-V2', got $HTTP_CODE + '${CONTENT:0:60}'"

                # Hang-up mid-PUT. Announce 1 MB, send 192 KB -- several
                # 64 KB dispatcher chunks, so write_chunk has written
                # bytes -- and hold the connection open: the PUT is IN
                # FLIGHT.
                DAV_HOSTPORT="${BASE#http://}"
                if exec 7<>"/dev/tcp/${DAV_HOSTPORT%%:*}/${DAV_HOSTPORT##*:}"; then
                    printf 'PUT /dav/fs0/atomic.txt HTTP/1.1\r\nHost: %s\r\nContent-Length: 1048576\r\n\r\n' \
                        "$DAV_HOSTPORT" >&7
                    head -c 196608 /dev/zero | tr '\0' 'X' >&7
                    sleep 2

                    # Control: the PUT really is in flight -- a mount
                    # takes one PUT at a time and refuses a second 503.
                    HTTP_CODE=$(printf '%s' "second" | \
                        curl -s -o /dev/null -w "%{http_code}" -T - \
                        "$BASE/dav/fs0/second.txt" 2>/dev/null || true)
                    [ "$HTTP_CODE" = "503" ] && \
                        pass "serve dav atomic: a PUT is in flight (second PUT 503)" || \
                        fail "serve dav atomic: in-flight control" \
                             "expected 503, got $HTTP_CODE"

                    # While it streams, the target still holds the old
                    # bytes -- nothing was truncated at write_open.
                    CONTENT=$(curl -sf "$ATOMIC_URL" 2>/dev/null || true)
                    [ "$CONTENT" = "SHORT-V2" ] && \
                        pass "serve dav atomic: target untouched while a PUT streams" || \
                        fail "serve dav atomic: target during PUT" \
                             "expected 'SHORT-V2', got '${CONTENT:0:60}'"

                    # The in-flight temp is in no listing.
                    LISTING=$(curl -s -X PROPFIND -H "Depth: 1" \
                        "$BASE/dav/fs0/" 2>/dev/null || true)
                    if echo "$LISTING" | grep -q "atomic.txt" && \
                       ! echo "$LISTING" | grep -qi "~AP"; then
                        pass "serve dav atomic: PROPFIND hides the in-flight temp"
                    else
                        fail "serve dav atomic: PROPFIND during PUT" \
                             "expected atomic.txt and no ~AP temp, got: ${LISTING:0:300}"
                    fi
                    LISTING=$(curl -s -H "Accept: application/json" \
                        "$BASE/fs0/" 2>/dev/null || true)
                    if echo "$LISTING" | grep -q "atomic.txt" && \
                       ! echo "$LISTING" | grep -qi "~AP"; then
                        pass "serve dav atomic: REST listing hides the in-flight temp"
                    else
                        fail "serve dav atomic: REST listing during PUT" \
                             "expected atomic.txt and no ~AP temp, got: ${LISTING:0:300}"
                    fi

                    # A live temp is no resource on ANY surface: a
                    # request naming it finds nothing. Its name comes
                    # from the -v log line the PUT's open writes.
                    TEMP_NAME=$(grep -aoE "streams to temp ~AP[0-9A-F]{5}\.TMP" \
                        "$SERIAL_LOG" 2>/dev/null | tail -1 | grep -oE "~AP[0-9A-F]{5}\.TMP" || true)
                    if [ -z "$TEMP_NAME" ]; then
                        fail "serve live temp: name" "no 'streams to temp' line in the -v log"
                    else
                        pass "serve live temp: the -v log names the PUT's temp"
                        for LT in "GET|$BASE/fs0/$TEMP_NAME" \
                                  "HEAD|$BASE/fs0/$TEMP_NAME" \
                                  "DELETE|$BASE/fs0/$TEMP_NAME" \
                                  "GET|$BASE/dav/fs0/$TEMP_NAME" \
                                  "DELETE|$BASE/dav/fs0/$TEMP_NAME"; do
                            LT_METHOD="${LT%%|*}"
                            LT_URL="${LT#*|}"
                            if [ "$LT_METHOD" = "HEAD" ]; then
                                HTTP_CODE=$(curl -s -o /dev/null -I -w "%{http_code}" \
                                    "$LT_URL" 2>/dev/null || true)
                            else
                                HTTP_CODE=$(curl -s -o /dev/null -X "$LT_METHOD" \
                                    -w "%{http_code}" "$LT_URL" 2>/dev/null || true)
                            fi
                            [ "$HTTP_CODE" = "404" ] && \
                                pass "serve live temp: $LT_METHOD ${LT_URL#$BASE} is 404" || \
                                fail "serve live temp: $LT_METHOD ${LT_URL#$BASE}" \
                                     "expected 404, got $HTTP_CODE"
                        done
                        HTTP_CODE=$(printf '%s' "over the temp" | \
                            curl -s -o /dev/null -w "%{http_code}" -T - \
                            "$BASE/fs0/$TEMP_NAME" 2>/dev/null || true)
                        [ "$HTTP_CODE" = "403" ] && \
                            pass "serve live temp: REST PUT onto it is 403" || \
                            fail "serve live temp: REST PUT onto it" \
                                 "expected 403, got $HTTP_CODE"
                        HTTP_CODE=$(curl -s -o /dev/null -X POST -w "%{http_code}" \
                            "$BASE/fs0/$TEMP_NAME?mkdir" 2>/dev/null || true)
                        [ "$HTTP_CODE" = "404" ] && \
                            pass "serve live temp: REST ?mkdir of it is 404" || \
                            fail "serve live temp: REST ?mkdir of it" \
                                 "expected 404, got $HTTP_CODE"
                        # FAT trims a leading blank, and trailing blanks
                        # and periods, from a name before it looks it up,
                        # so "<temp>." and "<temp> " would open the temp;
                        # no FAT name can start with a blank or end with a
                        # blank or period, so such a component is 400.
                        # Every other spelling of the temp is refused too:
                        # a backslash or ':' is UEFI path syntax, not a
                        # name byte, so a REST path decoding to one is 400;
                        # a trailing slash, or another letter case, still
                        # names the temp (404). And a /dav MOVE may not
                        # land on it under any spelling (403), moving
                        # nothing.
                        TEMP_LC=$(echo "$TEMP_NAME" | tr 'A-Z' 'a-z')
                        for LT in "GET|/fs0/%5C$TEMP_NAME|400" \
                                  "GET|/fs0/.%5C$TEMP_NAME|400" \
                                  "DELETE|/fs0/.%5C$TEMP_NAME|400" \
                                  "GET|/fs0/fs0%3A$TEMP_NAME|400" \
                                  "GET|/fs0/$TEMP_NAME/|404" \
                                  "DELETE|/fs0/$TEMP_NAME/|404" \
                                  "GET|/fs0/$TEMP_LC|404" \
                                  "DELETE|/fs0/$TEMP_LC|404" \
                                  "GET|/fs0/$TEMP_NAME.|400" \
                                  "HEAD|/fs0/$TEMP_NAME.|400" \
                                  "GET|/fs0/$TEMP_NAME%20|400" \
                                  "DELETE|/fs0/%20$TEMP_NAME|400" \
                                  "DELETE|/fs0/$TEMP_NAME.|400"; do
                            LT_METHOD="${LT%%|*}"
                            LT_REST="${LT#*|}"
                            LT_PATH="${LT_REST%%|*}"
                            LT_WANT="${LT_REST##*|}"
                            if [ "$LT_METHOD" = "HEAD" ]; then
                                HTTP_CODE=$(curl -s -o /dev/null -I \
                                    -w "%{http_code}" "$BASE$LT_PATH" 2>/dev/null || true)
                            else
                                HTTP_CODE=$(curl -s -o /dev/null -X "$LT_METHOD" \
                                    -w "%{http_code}" "$BASE$LT_PATH" 2>/dev/null || true)
                            fi
                            [ "$HTTP_CODE" = "$LT_WANT" ] && \
                                pass "serve live temp: $LT_METHOD $LT_PATH is $LT_WANT" || \
                                fail "serve live temp: $LT_METHOD $LT_PATH" \
                                     "expected $LT_WANT, got $HTTP_CODE"
                        done
                        printf '%s' "mvsrc" | curl -s -o /dev/null -T - \
                            "$BASE/fs0/mv-src.txt" 2>/dev/null || true
                        # A raw '\\' or a FAT-trimmed component in the
                        # Destination is refused by the SDK itself (400,
                        # an unparsable Destination) before the op; the
                        # rest reach webfs's MOVE, which refuses them 403.
                        for LT_MV in "403|$BASE/dav/fs0/$TEMP_NAME/" \
                                     "400|$BASE/dav/fs0/.\\$TEMP_NAME" \
                                     "403|$BASE/dav/fs0/$TEMP_LC" \
                                     "400|$BASE/dav/fs0/$TEMP_NAME."; do
                            LT_WANT="${LT_MV%%|*}"
                            LT_DST="${LT_MV#*|}"
                            HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" \
                                -X MOVE -H "Destination: $LT_DST" -H "Overwrite: T" \
                                "$BASE/dav/fs0/mv-src.txt" 2>/dev/null || true)
                            [ "$HTTP_CODE" = "$LT_WANT" ] && \
                                pass "serve live temp: MOVE onto ${LT_DST#$BASE} is $LT_WANT" || \
                                fail "serve live temp: MOVE onto ${LT_DST#$BASE}" \
                                     "expected $LT_WANT, got $HTTP_CODE"
                        done
                        CONTENT=$(curl -sf "$BASE/fs0/mv-src.txt" 2>/dev/null || true)
                        [ "$CONTENT" = "mvsrc" ] && \
                            pass "serve live temp: no refused MOVE moved its source" || \
                            fail "serve live temp: MOVE source" "got '${CONTENT:0:60}'"
                    fi

                    exec 7>&-
                    sleep 2
                    CONTENT=$(curl -sf "$ATOMIC_URL" 2>/dev/null || true)
                    [ "$CONTENT" = "SHORT-V2" ] && \
                        pass "serve dav atomic: hang-up mid-PUT keeps the old file" || \
                        fail "serve dav atomic: hang-up mid-PUT" \
                             "expected 'SHORT-V2', got '${CONTENT:0:60}'"
                    LISTING=$(curl -s -H "Accept: application/json" \
                        "$BASE/fs0/" 2>/dev/null || true)
                    if echo "$LISTING" | grep -q "atomic.txt" && \
                       ! echo "$LISTING" | grep -qi "~AP"; then
                        pass "serve dav atomic: the discarded temp is removed"
                    else
                        fail "serve dav atomic: temp after hang-up" \
                             "expected atomic.txt and no ~AP file, got: ${LISTING:0:300}"
                    fi
                else
                    fail "serve dav atomic: hang-up mid-PUT" \
                         "could not connect to $DAV_HOSTPORT"
                fi

                # ----------------------------------------------------
                # The REST PUT is atomic too: same temp + replace as
                # /dav, the same discard on a hang-up or a digest
                # mismatch, the same digest-cache invalidation.
                # ----------------------------------------------------
                REST_URL="$BASE/fs0/rest-atomic.txt"
                printf '%s' "the original, longer REST contents" | \
                    curl -s -o /dev/null -T - "$REST_URL" 2>/dev/null || true
                HTTP_CODE=$(printf '%s' "rest-v1" | \
                    curl -s -o /dev/null -w "%{http_code}" -T - \
                    "$REST_URL" 2>/dev/null || true)
                CONTENT=$(curl -sf "$REST_URL" 2>/dev/null || true)
                [ "$HTTP_CODE" = "201" ] && [ "$CONTENT" = "rest-v1" ] && \
                    pass "serve rest atomic: a shorter replace leaves no stale tail" || \
                    fail "serve rest atomic: shorter replace" \
                         "expected 201 + 'rest-v1', got $HTTP_CODE + '${CONTENT:0:60}'"

                # Prime the digest cache, then replace with a same-size body.
                curl -sI -H "Want-Digest: sha-256" "$REST_URL" >/dev/null 2>&1 || true
                printf '%s' "REST-V2" | curl -s -o /dev/null -T - "$REST_URL" \
                    2>/dev/null || true
                WANT_HEX=$(printf '%s' "REST-V2" | sha256sum | cut -d' ' -f1)
                GOT_DIGEST=$(curl -sI -H "Want-Digest: sha-256" "$REST_URL" \
                    2>/dev/null | tr -d '\r' | grep -i '^Digest:' || true)
                [ "$GOT_DIGEST" = "Digest: sha-256=$WANT_HEX" ] && \
                    pass "serve rest atomic: a same-size replace serves a fresh digest" || \
                    fail "serve rest atomic: digest after replace" \
                         "expected 'Digest: sha-256=$WANT_HEX', got '$GOT_DIGEST'"

                HTTP_CODE=$(printf '%s' "a REST body that must not land" | \
                    curl -s -o /dev/null -w "%{http_code}" \
                    -H "Content-Digest: sha-256=0000000000000000000000000000000000000000000000000000000000000000" \
                    -T - "$REST_URL" 2>/dev/null || true)
                CONTENT=$(curl -sf "$REST_URL" 2>/dev/null || true)
                [ "$HTTP_CODE" = "400" ] && [ "$CONTENT" = "REST-V2" ] && \
                    pass "serve rest atomic: digest-mismatch PUT (400) keeps the old file" || \
                    fail "serve rest atomic: digest mismatch" \
                         "expected 400 + 'REST-V2', got $HTTP_CODE + '${CONTENT:0:60}'"
                LISTING=$(curl -s -H "Accept: application/json" \
                    "$BASE/fs0/" 2>/dev/null || true)
                if echo "$LISTING" | grep -q "rest-atomic.txt" && \
                   ! echo "$LISTING" | grep -qi "~AP"; then
                    pass "serve rest atomic: the mismatched PUT's temp is removed"
                else
                    fail "serve rest atomic: temp after mismatch" \
                         "expected rest-atomic.txt and no ~AP file, got: ${LISTING:0:300}"
                fi

                if exec 7<>"/dev/tcp/${DAV_HOSTPORT%%:*}/${DAV_HOSTPORT##*:}"; then
                    printf 'PUT /fs0/rest-atomic.txt HTTP/1.1\r\nHost: %s\r\nContent-Length: 1048576\r\n\r\n' \
                        "$DAV_HOSTPORT" >&7
                    head -c 196608 /dev/zero | tr '\0' 'X' >&7
                    sleep 2
                    CONTENT=$(curl -sf "$REST_URL" 2>/dev/null || true)
                    [ "$CONTENT" = "REST-V2" ] && \
                        pass "serve rest atomic: target untouched while a PUT streams" || \
                        fail "serve rest atomic: target during PUT" \
                             "expected 'REST-V2', got '${CONTENT:0:60}'"
                    exec 7>&-
                    sleep 2
                    CONTENT=$(curl -sf "$REST_URL" 2>/dev/null || true)
                    [ "$CONTENT" = "REST-V2" ] && \
                        pass "serve rest atomic: hang-up mid-PUT keeps the old file" || \
                        fail "serve rest atomic: hang-up mid-PUT" \
                             "expected 'REST-V2', got '${CONTENT:0:60}'"
                    LISTING=$(curl -s -H "Accept: application/json" \
                        "$BASE/fs0/" 2>/dev/null || true)
                    if echo "$LISTING" | grep -q "rest-atomic.txt" && \
                       ! echo "$LISTING" | grep -qi "~AP"; then
                        pass "serve rest atomic: the hung-up PUT's temp is removed"
                    else
                        fail "serve rest atomic: temp after hang-up" \
                             "expected rest-atomic.txt and no ~AP file, got: ${LISTING:0:300}"
                    fi
                else
                    fail "serve rest atomic: hang-up mid-PUT" \
                         "could not connect to $DAV_HOSTPORT"
                fi

                # ----------------------------------------------------
                # Overlapping REST PUTs. The upload state belongs to ONE
                # request (its request_id): a second PUT while one is
                # in flight is 503 + Retry-After: 1 and touches nothing
                # (as /dav), and an abort from another connection does
                # not discard the PUT in flight.
                # ----------------------------------------------------
                OV_BODY=$(mktemp)
                head -c 1048576 /dev/urandom > "$OV_BODY"
                if exec 7<>"/dev/tcp/${DAV_HOSTPORT%%:*}/${DAV_HOSTPORT##*:}"; then
                    printf 'PUT /fs0/ov-a.bin HTTP/1.1\r\nHost: %s\r\nContent-Length: 1048576\r\nConnection: close\r\n\r\n' \
                        "$DAV_HOSTPORT" >&7
                    head -c 196608 "$OV_BODY" >&7
                    sleep 2

                    # B overlaps A: refused, and nothing of it lands.
                    OV_HDR=$(mktemp)
                    HTTP_CODE=$(printf '%s' "hello-b" | curl -s -o /dev/null \
                        -D "$OV_HDR" -w "%{http_code}" -T - \
                        "$BASE/fs0/ov-b.txt" 2>/dev/null || true)
                    OV_RETRY=$(tr -d '\r' < "$OV_HDR" | grep -i '^Retry-After:' || true)
                    [ "$HTTP_CODE" = "503" ] && [ "$OV_RETRY" = "Retry-After: 1" ] && \
                        pass "serve rest overlap: a second PUT is 503 with Retry-After: 1" || \
                        fail "serve rest overlap: second PUT" \
                             "expected 503 + 'Retry-After: 1', got $HTTP_CODE + '$OV_RETRY'"
                    rm -f "$OV_HDR"

                    # C hangs up before sending any body: its abort must
                    # not discard A.
                    if exec 8<>"/dev/tcp/${DAV_HOSTPORT%%:*}/${DAV_HOSTPORT##*:}"; then
                        printf 'PUT /fs0/ov-c.txt HTTP/1.1\r\nHost: %s\r\nContent-Length: 4096\r\n\r\n' \
                            "$DAV_HOSTPORT" >&8
                        sleep 1
                        exec 8>&-
                        sleep 1
                    fi

                    # A finishes: 201, and exactly its bytes.
                    tail -c +196609 "$OV_BODY" >&7
                    OV_STATUS=$(timeout 10 head -n 1 <&7 | tr -d '\r' || true)
                    exec 7>&-
                    [ "$OV_STATUS" = "HTTP/1.1 201 Created" ] && \
                        pass "serve rest overlap: the PUT in flight still answers 201" || \
                        fail "serve rest overlap: first PUT" \
                             "expected 'HTTP/1.1 201 Created', got '$OV_STATUS'"
                    OV_GOT=$(mktemp)
                    curl -sf "$BASE/fs0/ov-a.bin" -o "$OV_GOT" 2>/dev/null || true
                    cmp -s "$OV_BODY" "$OV_GOT" && \
                        pass "serve rest overlap: the first file holds exactly its body" || \
                        fail "serve rest overlap: first file" "content differs from its body"
                    rm -f "$OV_GOT"
                    for OV_NAME in ov-b.txt ov-c.txt; do
                        HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" \
                            "$BASE/fs0/$OV_NAME" 2>/dev/null || true)
                        [ "$HTTP_CODE" = "404" ] && \
                            pass "serve rest overlap: the refused $OV_NAME was not written" || \
                            fail "serve rest overlap: $OV_NAME" "expected 404, got $HTTP_CODE"
                    done

                    # B retried after A: stored whole.
                    HTTP_CODE=$(printf '%s' "hello-b" | curl -s -o /dev/null \
                        -w "%{http_code}" -T - "$BASE/fs0/ov-b.txt" 2>/dev/null || true)
                    CONTENT=$(curl -sf "$BASE/fs0/ov-b.txt" 2>/dev/null || true)
                    [ "$HTTP_CODE" = "201" ] && [ "$CONTENT" = "hello-b" ] && \
                        pass "serve rest overlap: the second file, retried, holds exactly its body" || \
                        fail "serve rest overlap: retried PUT" \
                             "expected 201 + 'hello-b', got $HTTP_CODE + '${CONTENT:0:60}'"
                else
                    fail "serve rest overlap" "could not connect to $DAV_HOSTPORT"
                fi
                rm -f "$OV_BODY"

                # A Range GET that also asks for the digest: the Digest
                # header goes into the table the Range response made,
                # under ITS ownership rules -- and the server keeps
                # answering afterwards.
                RANGE_HDR=$(mktemp)
                HTTP_CODE=$(curl -s -o /dev/null -D "$RANGE_HDR" -w "%{http_code}" \
                    -H "Range: bytes=0-2" -H "Want-Digest: sha-256" \
                    "$BASE/fs0/ov-b.txt" 2>/dev/null || true)
                WANT_HEX=$(printf '%s' "hello-b" | sha256sum | cut -d' ' -f1)
                RANGE_DIGEST=$(tr -d '\r' < "$RANGE_HDR" | grep -i '^Digest:' || true)
                [ "$HTTP_CODE" = "206" ] && [ "$RANGE_DIGEST" = "Digest: sha-256=$WANT_HEX" ] && \
                    pass "serve: a Range GET with Want-Digest is 206 with the whole file's Digest" || \
                    fail "serve: Range + Want-Digest" \
                         "expected 206 + 'Digest: sha-256=$WANT_HEX', got $HTTP_CODE + '$RANGE_DIGEST'"
                rm -f "$RANGE_HDR"
                CONTENT=$(curl -sf "$BASE/fs0/ov-b.txt" 2>/dev/null || true)
                [ "$CONTENT" = "hello-b" ] && \
                    pass "serve: the server still answers after a Range + Digest response" || \
                    fail "serve: after Range + Digest" "got '${CONTENT:0:60}'"

                # ----------------------------------------------------
                # Request paths are refused, never cut: a decoded path
                # past the limit is 414, an embedded NUL (%00) 400 --
                # and the %00 PUT does not write the name before it.
                # (The firmware's own FAT path limit, ~260 characters,
                # keeps a cut long path from being created here, so
                # what a cut would hit is only observable for %00.)
                # ----------------------------------------------------
                LP_LONG="$(printf 'd%.0s' $(seq 1 200))/$(printf 'e%.0s' $(seq 1 200))/$(printf 'f%.0s' $(seq 1 200))"
                HTTP_CODE=$(printf '%s' "long" | curl -s -o /dev/null -w "%{http_code}" \
                    -T - "$BASE/fs0/$LP_LONG" 2>/dev/null || true)
                [ "$HTTP_CODE" = "414" ] && \
                    pass "serve path: a REST PUT past the path limit is 414" || \
                    fail "serve path: long REST PUT" "expected 414, got $HTTP_CODE"
                HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" \
                    "$BASE/fs0/$LP_LONG" 2>/dev/null || true)
                [ "$HTTP_CODE" = "414" ] && \
                    pass "serve path: a REST GET past the path limit is 414" || \
                    fail "serve path: long REST GET" "expected 414, got $HTTP_CODE"
                HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X DELETE \
                    "$BASE/fs0/$LP_LONG" 2>/dev/null || true)
                [ "$HTTP_CODE" = "414" ] && \
                    pass "serve path: a REST DELETE past the path limit is 414" || \
                    fail "serve path: long REST DELETE" "expected 414, got $HTTP_CODE"

                HTTP_CODE=$(printf '%s' "nul" | curl -s -o /dev/null -w "%{http_code}" \
                    -T - "$BASE/fs0/nul.txt%00.evil" 2>/dev/null || true)
                [ "$HTTP_CODE" = "400" ] && \
                    pass "serve path: a REST path with %00 is 400" || \
                    fail "serve path: %00" "expected 400, got $HTTP_CODE"
                HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" \
                    "$BASE/fs0/nul.txt" 2>/dev/null || true)
                [ "$HTTP_CODE" = "404" ] && \
                    pass "serve path: the %00 PUT did not write the name before it" || \
                    fail "serve path: %00 cut" "expected 404, got $HTTP_CODE"

                # ----------------------------------------------------
                # Content-Digest on the REST PUT reads what /dav reads
                # (RFC 9530): sha-256 and sha-512, each as `:<base64>:`
                # or bare hex; the strongest listed is checked, an
                # unreadable value matches nothing, and a header naming
                # only other algorithms checks nothing.
                # ----------------------------------------------------
                DG_URL="$BASE/fs0/digest-rest.txt"
                b64_of() { printf '%s' "$2" | openssl dgst "-$1" -binary | base64 -w0; }
                hex_of() { printf '%s' "$2" | openssl dgst "-$1" -r | cut -d' ' -f1; }
                printf '%s' "dg-old" | curl -s -o /dev/null -T - "$DG_URL" 2>/dev/null || true
                for DG in "201|dg-v1|sha-256=:$(b64_of sha256 dg-v1):" \
                          "400|dg-x1|sha-256=:$(b64_of sha256 not-the-body):" \
                          "400|dg-x2|sha-512=$(hex_of sha512 not-the-body)" \
                          "201|dg-v2|sha-512=:$(b64_of sha512 dg-v2):" \
                          "201|dg-v3|sha-256=:$(b64_of sha256 wrong):, sha-512=:$(b64_of sha512 dg-v3):" \
                          "400|dg-x3|sha-256=:not base64:" \
                          "201|dg-v4|md5=:AAAAAAAAAAAAAAAAAAAAAA==:"; do
                    DG_WANT="${DG%%|*}"
                    DG_REST="${DG#*|}"
                    DG_BODY="${DG_REST%%|*}"
                    DG_HDR="${DG_REST#*|}"
                    DG_BEFORE=$(curl -sf "$DG_URL" 2>/dev/null || true)
                    HTTP_CODE=$(printf '%s' "$DG_BODY" | curl -s -o /dev/null \
                        -w "%{http_code}" -H "Content-Digest: $DG_HDR" -T - \
                        "$DG_URL" 2>/dev/null || true)
                    CONTENT=$(curl -sf "$DG_URL" 2>/dev/null || true)
                    if [ "$DG_WANT" = "201" ]; then DG_EXPECT="$DG_BODY"; else DG_EXPECT="$DG_BEFORE"; fi
                    [ "$HTTP_CODE" = "$DG_WANT" ] && [ "$CONTENT" = "$DG_EXPECT" ] && \
                        pass "serve rest digest: '${DG_HDR:0:40}' -> $DG_WANT" || \
                        fail "serve rest digest: '${DG_HDR:0:40}'" \
                             "expected $DG_WANT + '$DG_EXPECT', got $HTTP_CODE + '${CONTENT:0:60}'"
                done

                # ----------------------------------------------------
                # The REST PUT makes a missing parent (one level) for its
                # upload -- and a PUT that then fails, for any reason,
                # takes back the directory IT made (only while empty);
                # a parent that was already there stays.
                # ----------------------------------------------------
                dir_code() {
                    curl -s -o /dev/null -w "%{http_code}" \
                        -H "Accept: application/json" "$BASE/fs0/$1/" 2>/dev/null || true
                }
                BAD_DG="sha-256=0000000000000000000000000000000000000000000000000000000000000000"
                HTTP_CODE=$(printf '%s' "ok" | curl -s -o /dev/null -w "%{http_code}" \
                    -T - "$BASE/fs0/nd-ok/f.txt" 2>/dev/null || true)
                [ "$HTTP_CODE" = "201" ] && [ "$(dir_code nd-ok)" = "200" ] && \
                    pass "serve rest parent: a PUT makes its missing parent" || \
                    fail "serve rest parent: made" "expected 201 + dir 200, got $HTTP_CODE + $(dir_code nd-ok)"
                HTTP_CODE=$(printf '%s' "x" | curl -s -o /dev/null -w "%{http_code}" \
                    -H "Content-Digest: $BAD_DG" -T - "$BASE/fs0/nd-mis/f.txt" 2>/dev/null || true)
                [ "$HTTP_CODE" = "400" ] && [ "$(dir_code nd-mis)" = "404" ] && \
                    pass "serve rest parent: a mismatched PUT takes back the parent it made" || \
                    fail "serve rest parent: mismatch" "expected 400 + dir 404, got $HTTP_CODE + $(dir_code nd-mis)"
                HTTP_CODE=$(printf '%s' "x" | curl -s -o /dev/null -w "%{http_code}" \
                    -T - "$BASE/fs0/nd-ref/bad%3Cname" 2>/dev/null || true)
                [ "$HTTP_CODE" = "500" ] && [ "$(dir_code nd-ref)" = "404" ] && \
                    pass "serve rest parent: a refused PUT (500) takes back the parent it made" || \
                    fail "serve rest parent: refused" "expected 500 + dir 404, got $HTTP_CODE + $(dir_code nd-ref)"
                HTTP_CODE=$(printf '%s' "x" | curl -s -o /dev/null -w "%{http_code}" \
                    -H "Content-Digest: $BAD_DG" -T - "$BASE/fs0/nd-ok/g.txt" 2>/dev/null || true)
                [ "$HTTP_CODE" = "400" ] && [ "$(dir_code nd-ok)" = "200" ] && \
                    pass "serve rest parent: a failed PUT leaves a parent it did not make" || \
                    fail "serve rest parent: existing parent" "expected 400 + dir 200, got $HTTP_CODE + $(dir_code nd-ok)"
                if exec 7<>"/dev/tcp/${DAV_HOSTPORT%%:*}/${DAV_HOSTPORT##*:}"; then
                    printf 'PUT /fs0/nd-hup/f.txt HTTP/1.1\r\nHost: %s\r\nContent-Length: 1048576\r\n\r\n' \
                        "$DAV_HOSTPORT" >&7
                    head -c 196608 /dev/zero >&7
                    sleep 2
                    exec 7>&-
                    sleep 2
                    [ "$(dir_code nd-hup)" = "404" ] && \
                        pass "serve rest parent: a hung-up PUT takes back the parent it made" || \
                        fail "serve rest parent: hang-up" "expected dir 404, got $(dir_code nd-hup)"
                else
                    fail "serve rest parent: hang-up" "could not connect to $DAV_HOSTPORT"
                fi

                # ----------------------------------------------------
                # REST PUT statuses, exact: a parent missing more than
                # one level down is 409 (and makes nothing); a
                # directory at the target is 409; an unknown volume is
                # 404. (507 needs a full volume; not provoked here.)
                # ----------------------------------------------------
                for ST in "409|/fs0/nope1/nope2/f.txt" "409|/fs0/EFI" "404|/fs99/f.txt"; do
                    ST_WANT="${ST%%|*}"
                    ST_PATH="${ST#*|}"
                    HTTP_CODE=$(printf '%s' "x" | curl -s -o /dev/null -w "%{http_code}" \
                        -T - "$BASE$ST_PATH" 2>/dev/null || true)
                    [ "$HTTP_CODE" = "$ST_WANT" ] && \
                        pass "serve rest status: PUT $ST_PATH is $ST_WANT" || \
                        fail "serve rest status: PUT $ST_PATH" \
                             "expected $ST_WANT, got $HTTP_CODE"
                done
                HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" \
                    -H "Accept: application/json" "$BASE/fs0/nope1/" 2>/dev/null || true)
                [ "$HTTP_CODE" = "404" ] && \
                    pass "serve rest status: the 409 PUT made no directory" || \
                    fail "serve rest status: nope1" "expected 404, got $HTTP_CODE"


                # ----------------------------------------------------
                # The mount client against webfs serve itself: a second
                # guest mounts this guest's /dav/fs0/ (a subpath, through
                # the host's port forward) and lists, reads and writes it
                # -- both ends on the SDK's HTTP client and server.
                # ----------------------------------------------------
                IO_DIR=$(mktemp -d)
                printf 'through the mount\n' > "$IO_DIR/io-src.txt"
                IO_NSH=$(mktemp --suffix=.nsh)
                cat > "$IO_NSH" <<NSHEOF
@echo -off
fs0:
axl-webfs.efi mount --protocol dav http://10.0.2.2:${DETACH_PORT}/dav/fs0/
map -r
echo === LS ===
ls fs1:\\
echo === TYPE ===
type fs1:\\detach_test.txt
echo === PUT ===
cp fs0:\\io-src.txt fs1:\\io-through.txt
echo === END ===
axl-webfs.efi umount
reset -s
NSHEOF
                IO_LOG=$(mktemp)
                "$RUN_QEMU_SH" --arch X64 --timeout 90 --raw --net \
                    --extra "$IO_DIR/io-src.txt" \
                    --nsh "$IO_NSH" --serial-log "$IO_LOG" \
                    "$APP_EFI" > /dev/null 2>&1 || true
                rm -f "$IO_NSH"
                # The share's root listing, exact: detach_test.txt's row
                # (12 bytes; the mount reports no time), and no row for the
                # share itself ("fs0", the PROPFIND self entry).
                IO_LS=$(sed -n '/=== LS ===/,/=== TYPE ===/p' "$IO_LOG" | tr -d '\r')
                IO_ROW=$(printf '00/00/0000  00:00%20s  detach_test.txt' 12)
                if echo "$IO_LS" | grep -qxF "$IO_ROW" && \
                   ! echo "$IO_LS" | grep -qE "  fs0$"; then
                    pass "serve interop: the share's root lists detach_test.txt exactly, and not itself"
                else
                    fail "serve interop: ls" "expected the row '$IO_ROW' and no 'fs0' row in: ${IO_LS:0:300}"
                fi
                IO_TYPE=$(sed -n '/=== TYPE ===/,/=== PUT ===/p' "$IO_LOG" | tr -d '\r' | \
                    grep -x "detach test" || true)
                [ "$IO_TYPE" = "detach test" ] && \
                    pass "serve interop: a mount of /dav/fs0/ reads a served file" || \
                    fail "serve interop: read" "no 'detach test' line from type: $(grep -a 'axl-webfs-mount' "$IO_LOG" | tr -d '\r' | tr '\n' ' ' | cut -c1-200)"
                CONTENT=$(curl -sf "$BASE/fs0/io-through.txt" 2>/dev/null | tr -d '\r' || true)
                [ "$CONTENT" = "through the mount" ] && \
                    pass "serve interop: a file written through the mount lands on the served volume" || \
                    fail "serve interop: write" "expected 'through the mount', got '${CONTENT:0:60}'"
                rm -rf "$IO_LOG" "$IO_DIR"

                # The guest must still be running here: one that died
                # early (a crash, its own timeout) would otherwise only
                # show as the checks above failing with 000 -- and the
                # teardown's `kill || true` would hide that it was gone.
                if kill -0 "$QEMU_PID" 2>/dev/null && \
                   [ "$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 "$BASE/" 2>/dev/null)" = "200" ]; then
                    pass "serve: the guest is alive and answering at the end of the block"
                else
                    fail "serve: guest liveness" "the guest died or stopped answering before teardown"
                fi
            else
                fail "serve: driver did not start within 30s"
            fi

            kill "$QEMU_PID" 2>/dev/null || true; wait "$QEMU_PID" 2>/dev/null || true
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

            kill "$QEMU_PID" 2>/dev/null || true; wait "$QEMU_PID" 2>/dev/null || true
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
