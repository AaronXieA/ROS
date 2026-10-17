#!/bin/sh
# mkpck.sh - build a PCK1 package for ROS Command's pck manager.
# Usage: mkpck.sh OUT.pck FILE [FILE...]
#
# PCK1 format:
#   "PCK1"            4 bytes magic
#   dw count          number of entries (little-endian)
#   per entry:
#     11 bytes        FAT 8.3 name (padded with spaces)
#     dd size         content size (little-endian)
#     content bytes
set -e

out=$1; shift
[ $# -gt 0 ] || { echo "usage: mkpck.sh out.pck file..." >&2; exit 1; }

printf 'PCK1' > "$out"
printf "$(printf '\\x%02x\\x%02x' $(($# & 255)) $(($# >> 8)))" >> "$out"

for f in "$@"; do
    base=$(basename "$f" | tr 'a-z' 'A-Z')
    name=${base%%.*}
    ext=''
    case $base in *.*) ext=${base##*.} ;; esac
    printf '%-8.8s%-3.3s' "$name" "$ext" >> "$out"
    size=$(stat -f%z "$f")
    printf "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' \
        $((size & 255)) $(((size >> 8) & 255)) \
        $(((size >> 16) & 255)) $(((size >> 24) & 255)))" >> "$out"
    cat "$f" >> "$out"
done

echo "OK: $out ($(stat -f%z "$out") bytes, $# file(s))"
