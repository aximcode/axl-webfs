#!/usr/bin/env bash
# Copyright 2026 AximCode
# SPDX-License-Identifier: Apache-2.0
#
# Regenerate docs/assets/demo-serve.gif.
#
# Boots axl-webfs.efi serve in QEMU with port forwarding, screenshots
# each page of the navigation tree with headless Chrome, stitches the
# frames into a GIF with ffmpeg. Uses the real HTML output — no mocks.
#
# Prereqs: axl-webfs.efi built; run-qemu.sh on AXL_SDK_SRC; qemu-system-x86_64
# discoverable by axl-common.sh (via $QEMU_DIR or PATH); google-chrome, ffmpeg.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

: "${AXL_SDK_SRC:?AXL_SDK_SRC must point to an axl-sdk checkout (for run-qemu.sh)}"
RUN_QEMU="$AXL_SDK_SRC/scripts/run-qemu.sh"
[ -x "$RUN_QEMU" ] || { echo "run-qemu.sh not found at $RUN_QEMU" >&2; exit 1; }

APP_EFI="$PROJECT_ROOT/build/axl/x64/axl-webfs.efi"
[ -f "$APP_EFI" ] || { echo "build $APP_EFI first (make)" >&2; exit 1; }

HOST_PORT=18080
OUT_GIF="$PROJECT_ROOT/docs/assets/demo-serve.gif"
FRAMES_DIR="$(mktemp -d)"
trap 'rm -rf "$FRAMES_DIR"; [ -n "${QEMU_PID:-}" ] && kill "$QEMU_PID" 2>/dev/null || true' EXIT

echo "[demo-serve] booting QEMU with axl-webfs.efi serve ..."
QEMU_OUT=$(
    "$RUN_QEMU" --net --hostfwd "$HOST_PORT:8080" --background --timeout 180 \
        "$APP_EFI" serve
)
QEMU_PID=$(echo "$QEMU_OUT" | grep -oE 'QEMU_PID=[0-9]+' | head -1 | cut -d= -f2)
echo "[demo-serve] qemu pid=$QEMU_PID; waiting for HTTP ..."

for i in $(seq 1 30); do
    sleep 2
    if curl -s -m 2 -o /dev/null -w '%{http_code}' \
            "http://localhost:$HOST_PORT/" 2>/dev/null | grep -q 200; then
        echo "[demo-serve] server up after $((i * 2))s"
        break
    fi
done

URLS=(
    "http://localhost:$HOST_PORT/"
    "http://localhost:$HOST_PORT/fs0/"
    "http://localhost:$HOST_PORT/fs0/EFI/"
    "http://localhost:$HOST_PORT/fs0/EFI/Boot/"
)
i=0
for url in "${URLS[@]}"; do
    echo "[demo-serve]   $(printf '%02d' $i)  $url"
    google-chrome --headless=new --disable-gpu --no-sandbox \
        --hide-scrollbars --window-size=1200,420 \
        --screenshot="$FRAMES_DIR/$(printf %02d $i).png" "$url" 2>/dev/null
    i=$((i + 1))
done

cat > "$FRAMES_DIR/list.txt" <<EOF
file '00.png'
duration 2.0
file '01.png'
duration 2.0
file '02.png'
duration 2.0
file '03.png'
duration 3.0
file '03.png'
EOF

echo "[demo-serve] stitching $OUT_GIF ..."
cd "$FRAMES_DIR"
ffmpeg -y -loglevel error -f concat -safe 0 -i list.txt \
    -vf "fps=12,scale=1200:-1:flags=lanczos,split[s0][s1];[s0]palettegen=max_colors=96[p];[s1][p]paletteuse=dither=bayer:bayer_scale=4" \
    -loop 0 "$OUT_GIF"

ls -la "$OUT_GIF"
echo "[demo-serve] done."
