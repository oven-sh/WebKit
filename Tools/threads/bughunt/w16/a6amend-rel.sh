#!/bin/bash
# A6 amend round: quiet-host Release gate, EXACT pinned invocation, n=12.
# Archives EVERY log + a per-run RC/classification summary line (review
# finding: passing-run evidence must be archived, not deleted).
set -u
cd /root/WebKit
OUT=/root/WebKit/Tools/threads/bughunt/w16/rounds/a6amend-rel
mkdir -p "$OUT"
SUM="$OUT/SUMMARY.txt"
: > "$SUM"
GILOFF="--useJSThreads=1 --useThreadGIL=0 --useVMLite=1 --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1"
# Quiet-host precondition recorded up front (review finding: prior gates ran
# against a concurrent W=16 bench from /root/WebKit-limp).
{ echo "host precondition:"; uptime; pgrep -af "bin/jsc" | grep -v $$ || echo "  no foreign jsc"; } >> "$SUM"
for i in $(seq 1 12); do
  log="$OUT/rel-$i.log"
  timeout 2400 WebKitBuild/Release/bin/jsc $GILOFF Tools/threads/scalebench/js/bench.js -- 16 > "$log" 2>&1
  rc=$?
  cls="UNCLASSIFIED"
  if [ "$rc" = "0" ]; then cls="PASS"
  elif grep -q "pas panic" "$log"; then cls="pas-alloc-bit (open SPEC-jit family)"
  elif grep -q "stop-the-world failed" "$log"; then cls="STW-watchdog-30s (ab17b family)"
  elif [ ! -s "$log" ]; then cls="EMPTY-LOG"
  fi
  echo "rel-$i rc=$rc bytes=$(stat -c%s "$log") class=$cls" >> "$SUM"
done
echo "A6AMEND-REL-DONE" >> "$SUM"
