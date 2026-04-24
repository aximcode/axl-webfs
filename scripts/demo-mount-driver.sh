#!/usr/bin/env bash
# Copyright 2026 AximCode
# SPDX-License-Identifier: Apache-2.0
#
# Drives the mount demo for docs/assets/demo-mount.gif.
# Creates a tmux split-pane session, runs a narrative script in each
# pane with deterministic sleeps, attaches in the foreground so vhs
# records a deterministic GIF.
#
# The output is a scripted reproduction of what the real tools print;
# see src/app/cmd-mount.c and src/driver/webfs.c for the string
# sources. No real xfer-server or UEFI host is contacted.

set -eu

SESSION=webfs-demo
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

tmux kill-session -t "$SESSION" 2>/dev/null || true

TMUX_CONF=$(mktemp)
cat > "$TMUX_CONF" <<'EOF'
set -g status off
set -g pane-border-style        "fg=#3a3f4a"
set -g pane-active-border-style "fg=#3a3f4a"
set -g default-terminal         "xterm-256color"
set -g escape-time              0
EOF

tmux -f "$TMUX_CONF" new-session  -d -s "$SESSION" -x 200 -y 17 \
    "bash '$SELF_DIR/demo-pane-workstation.sh'"
tmux -f "$TMUX_CONF" split-window -h -t "$SESSION" \
    "bash '$SELF_DIR/demo-pane-uefi.sh'"

exec tmux -f "$TMUX_CONF" attach -t "$SESSION"
