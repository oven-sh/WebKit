#!/usr/bin/env bash
# mkreports.sh <round-dir> <out-log> — concatenate all TSAN warning blocks
# from per-test logs into one reports log (same format as reports-r0..r3.log).
set -u
DIR=$1; OUT=$2
: > "$OUT"
for f in $(ls "$DIR"/*.log | sort); do
  echo "===== SOURCE TEST LOG: $(basename "$(dirname "$f")")/$(basename "$f") =====" >> "$OUT"
  awk '/WARNING: ThreadSanitizer/{p=1} p{print} /^==================$/{if(p){p=0}}' "$f" >> "$OUT"
done
echo "reports: $(grep -c 'WARNING: ThreadSanitizer' "$OUT")"
