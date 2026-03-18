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
while [[ $# -gt 0 ]]; do
    case $1 in
        --qemu) RUN_QEMU=true; shift ;;
        --help|-h)
            echo "Usage: $0 [--qemu]"
            echo "  --qemu   Also run QEMU integration tests"
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
    info "QEMU" "Integration tests not yet implemented (Phase 5)"
    skip "QEMU integration tests"
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
