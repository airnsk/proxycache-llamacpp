#!/bin/bash
# Cold vs warm read of a disk-cache state file (project llama-disk-cache).
# O_DIRECT bypasses the page cache -> real device throughput; a plain read after that is "warm".
# usage: bash cold_read.sh <file>
F="${1:?file}"
SZ=$(stat -c %s "$F")
echo "file=$F size=$SZ"
echo "--- O_DIRECT (bypasses page cache) ---"
dd if="$F" of=/dev/null bs=8M iflag=direct 2>&1 | tail -1
echo "--- normal read (page cache may serve it) ---"
dd if="$F" of=/dev/null bs=8M 2>&1 | tail -1
echo "--- device ---"
df -h "$(dirname "$F")" | tail -1
