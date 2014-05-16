#!/usr/bin/env bash

# Test that bookmarking documents works.

# Run the script in the directory where this script and the test pdf
# documents are. A path to mupdf-x11 binary is required as an argument.
# Example:
# ./test_bookmarks.sh ../build/debug/mupdf-x11

# This needs xdotool.

function die_usage() {
    local program=$(basename $0)
    echo "usage: $program PATH_TO_MUPDF-X11" 1>&2
    exit 1
}

readonly BOOKMARK_FILE="$HOME/.mupdf_bookmarks"

function backup_bookmarks() {
    if [[ -e "$BOOKMARK_FILE" ]]; then
        echo "Backup $BOOKMARK_FILE to ${BOOKMARK_FILE}.bak"
        cp "$BOOKMARK_FILE" "${BOOKMARK_FILE}.bak"
        echo "Remove $BOOKMARK_FILE"
        rm "$BOOKMARK_FILE"
    fi
}

function send_keypresses_to_mupdf() {
    xdotool search --class --limit 1 mupdf windowactivate --sync
    xdotool key $1
}

function opened_pdf_page() {
    local name=$(xdotool search --class --limit 1 mupdf getwindowname)
    if ! [[ "$name" =~ " - $1 " ]]; then
        die "Didn't open second page of the pdf: $1"
    fi
}

function die() {
    echo "$*"
    exit 1
}

function bookmark_is_written_to_file() {
    local abs_path=$(readlink -f "$1")
    local found_bookmark=false

    exec 3< "$BOOKMARK_FILE"
    while read -u 3 -r line; do
        if [[ "$line" = "${abs_path} = 2" ]]; then
            found_bookmark=true
        fi
    done
    if ! "$found_bookmark"; then
        die "Didn't find bookmark for file: $abs_path"
    fi
    exec 3<&-
}

if [[ "$#" -ne 1 ]]; then
    die_usage
fi
readonly MUPDF="$1"

backup_bookmarks

for f in *.pdf; do
    "$MUPDF" "$f" &>/dev/null &
    sleep 0.5
    send_keypresses_to_mupdf "Right B q"
    bookmark_is_written_to_file "$f"

    "$MUPDF" "$f" &>/dev/null &
    sleep 0.5
    opened_pdf_page "2/2"
    send_keypresses_to_mupdf "q"
done
