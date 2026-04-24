#!/usr/bin/env bash
# Copyright 2026 AximCode
# SPDX-License-Identifier: Apache-2.0
#
# UEFI shell pane for the mount demo. Prints a scripted narrative
# that reproduces the real axl-webfs mount output; string sources:
# src/app/cmd-mount.c, src/driver/webfs.c.

set -eu

ORANGE=$'\e[38;5;214m'
DIM=$'\e[2m'
RESET=$'\e[0m'
PS1="${ORANGE}FS0:\\>${RESET} "

type_line() {
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
printf "${DIM}── UEFI shell ──${RESET}\n\n"
sleep 4.0

# 1) mount.
type_line "axl-webfs.efi mount http://10.0.0.5:8080/"
sleep 0.3
printf "Loading axl-webfs-dxe.efi...\n"
sleep 0.4
printf "Connecting to http://10.0.0.5:8080/...\n"
sleep 0.8
printf "axl-webfs-dxe: Mounted successfully\n"
printf "Mounted as FS handle A3FC8098\n"
printf "Use 'map -r' to refresh Shell mappings.\n"
sleep 1.2

# 2) ls fs1:.
type_line "ls fs1:"
sleep 0.3
printf 'Directory of: FS1:\\\n'
printf '  04/24/2026  10:14    342,016  IpmiTool.efi\n'
printf '  04/24/2026  10:14     12,288  README.txt\n'
sleep 1.5

# 3) run .efi directly off the mount.
type_line "fs1:\\IpmiTool.efi -h"
sleep 0.4
printf "IpmiTool v1.2 — IPMI command-line tool\n"
printf "Usage: IpmiTool <cmd> [args]\n"
sleep 2.5
printf "%s" "$PS1"
# Hold open past the end of the vhs recording window.
sleep 30
