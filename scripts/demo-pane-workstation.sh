#!/usr/bin/env bash
# Copyright 2026 AximCode
# SPDX-License-Identifier: Apache-2.0
#
# Workstation pane for the mount demo. Prints a scripted narrative
# with sleeps. Driven by demo-mount-driver.sh.

set -eu

# Colors.
GREEN=$'\e[38;5;114m'
DIM=$'\e[2m'
RESET=$'\e[0m'
PS1="${GREEN}ws${RESET}\$ "

type_line() {
    # $1 = text. Prints a line with a simulated typing cadence.
    local text="$1"
    printf "%s" "$PS1"
    local i=0
    while [ $i -lt ${#text} ]; do
        printf "%s" "${text:$i:1}"
        sleep 0.025
        i=$((i + 1))
    done
    printf "\n"
}

clear
printf "${DIM}── workstation ──${RESET}\n\n"
sleep 1.0

type_line "ls tools/"
sleep 0.2
printf "IpmiTool.efi  README.txt\n"
sleep 0.8

type_line "./xfer-server.py --root tools/"
sleep 0.2
printf "${DIM}[xfer-server] serving tools/ on :8080${RESET}\n"
sleep 2.7

# Aligned with UEFI "Connecting to ..." / mount-success block.
printf "${DIM}[xfer-server] 10.0.0.9 GET /                200${RESET}\n"
sleep 2.0

# Aligned with UEFI "ls fs1:".
printf "${DIM}[xfer-server] 10.0.0.9 GET /                200${RESET}\n"
sleep 2.5

# Aligned with UEFI "fs1:\IpmiTool.efi -h".
printf "${DIM}[xfer-server] 10.0.0.9 GET /IpmiTool.efi    200${RESET}\n"

# Hold open so tmux doesn't collapse the layout before vhs is done.
sleep 30
