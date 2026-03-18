#!/bin/bash

# UefiXfer — Client-side recursive file transfer helper.
# Wraps curl to upload/download entire directory trees from a UefiXfer serve instance.

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m'

show_usage() {
    echo "Usage: $0 {download|upload} <url> <local-path>"
    echo ""
    echo "Commands:"
    echo "  download <url> <local-dir>   Download directory tree from UefiXfer serve"
    echo "  upload   <local-dir> <url>   Upload directory tree to UefiXfer serve"
    echo ""
    echo "Examples:"
    echo "  $0 download http://192.168.1.100:8080/fs0/EFI/ ./local-copy/"
    echo "  $0 upload ./tools/ http://192.168.1.100:8080/fs0/tools/"
}

# Recursive download: GET directory listing (JSON), then download each file.
do_download() {
    local URL="$1"
    local LOCAL_DIR="$2"

    # Ensure URL ends with /
    [[ "$URL" != */ ]] && URL="${URL}/"

    mkdir -p "$LOCAL_DIR"

    # Get JSON directory listing
    local LISTING
    LISTING=$(curl -sf -H "Accept: application/json" "$URL" 2>/dev/null)
    if [ $? -ne 0 ]; then
        echo -e "${RED}ERROR:${NC} Cannot list $URL" >&2
        return 1
    fi

    # Parse JSON array and iterate entries
    echo "$LISTING" | python3 -c "
import sys, json
entries = json.load(sys.stdin)
for e in entries:
    print(f\"{e['name']}\t{'dir' if e.get('dir', False) else 'file'}\t{e.get('size', 0)}\")
" | while IFS=$'\t' read -r NAME TYPE SIZE; do
        if [ "$TYPE" = "dir" ]; then
            echo -e "${BLUE}DIR${NC}  $URL$NAME/"
            do_download "${URL}${NAME}/" "${LOCAL_DIR}/${NAME}"
        else
            echo -e "${GREEN}GET${NC}  $URL$NAME  ($SIZE bytes)"
            curl -sf "$URL$NAME" -o "${LOCAL_DIR}/${NAME}"
        fi
    done
}

# Recursive upload: walk local directory tree, PUT each file.
do_upload() {
    local LOCAL_DIR="$1"
    local URL="$2"

    # Ensure URL ends with /
    [[ "$URL" != */ ]] && URL="${URL}/"

    for ENTRY in "$LOCAL_DIR"/*; do
        [ -e "$ENTRY" ] || continue
        local NAME=$(basename "$ENTRY")

        if [ -d "$ENTRY" ]; then
            # Create remote directory
            echo -e "${BLUE}MKDIR${NC} ${URL}${NAME}/"
            curl -sf -X POST "${URL}${NAME}/?mkdir" > /dev/null 2>&1 || true
            do_upload "$ENTRY" "${URL}${NAME}"
        elif [ -f "$ENTRY" ]; then
            local SIZE=$(stat -c%s "$ENTRY")
            echo -e "${GREEN}PUT${NC}  ${URL}${NAME}  ($SIZE bytes)"
            curl -sf -T "$ENTRY" "${URL}${NAME}" > /dev/null
        fi
    done
}

# Main
if [ $# -lt 3 ]; then
    show_usage
    exit 1
fi

case "$1" in
    download)
        do_download "$2" "$3"
        echo -e "\n${GREEN}Download complete.${NC}"
        ;;
    upload)
        do_upload "$2" "$3"
        echo -e "\n${GREEN}Upload complete.${NC}"
        ;;
    *)
        show_usage
        exit 1
        ;;
esac
