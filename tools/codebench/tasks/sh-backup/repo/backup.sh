#!/bin/bash
# usage: backup.sh SOURCE_DIR DEST_DIR
src=$1
dst=$2
count=0
for f in `ls $src/*.txt`; do
    cp $f $dst
    count=$count+1
done
echo "copied $count files"
