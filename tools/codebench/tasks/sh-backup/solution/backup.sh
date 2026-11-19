#!/bin/bash
# usage: backup.sh SOURCE_DIR DEST_DIR
if [ "$#" -lt 2 ]; then
    echo "usage: backup.sh SOURCE_DIR DEST_DIR" >&2
    exit 2
fi
src=$1
dst=$2
if [ ! -d "$src" ]; then
    echo "backup.sh: source folder not found: $src" >&2
    exit 2
fi
mkdir -p "$dst"
count=0
for f in "$src"/*.txt; do
    [ -f "$f" ] || continue
    cp "$f" "$dst"/
    count=$((count + 1))
done
echo "copied $count files"
