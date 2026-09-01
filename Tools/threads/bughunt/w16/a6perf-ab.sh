#!/bin/bash
# A6 amend round V5b spot-check: flag-off LLInt-only varargs microbench A/B.
#   baseline = HEAD (5c0e51c2b543) Release jsc built from /root/wk-a6base
#   current  = working-tree Release jsc (/root/WebKit/WebKitBuild/Release)
# Flag-off (NO threads flags), --useJIT=0 to maximize the LLInt slow path the
# A6 echo touches. Interleaved A/B x10 each; checksums must be identical.
set -u
BASE=/root/wk-a6base/WebKitBuild/Release/bin/jsc
CUR=/root/WebKit/WebKitBuild/Release/bin/jsc
JS=/root/WebKit/Tools/threads/bughunt/w16/a6perf-spread.js
OUT=/root/WebKit/Tools/threads/bughunt/w16/rounds/a6amend-perf
mkdir -p "$OUT"
SUM="$OUT/SUMMARY.txt"
: > "$SUM"
{ echo "host precondition:"; uptime; } >> "$SUM"
for i in $(seq 1 10); do
  for arm in base cur; do
    bin=$BASE; [ "$arm" = cur ] && bin=$CUR
    o=$("$bin" --useJIT=0 "$JS" 2>&1)
    echo "$arm-$i $o" | tr '\n' ' ' >> "$SUM"; echo >> "$SUM"
  done
done
echo "A6AMEND-PERF-DONE" >> "$SUM"
