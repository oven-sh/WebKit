#!/bin/bash
# A6 amend round: ASAN Debug signature re-verify with the PINNED PROGRAM
# (Tools/threads/scalebench/js/bench.js), addressing the review finding that
# prior ASAN gates ran the bench-local.js harness instead.
# Attempt 1 (rounds/a6amend-asan-fullsize-attempt1) established that the
# FULL-SIZE pinned invocation under ASAN Debug JIT-on exceeds a 2400s
# timeout per run (3/3 rc=124, banner-only logs) — consistent with the
# original ledger note ("full-size ASAN runs (>35 min) were out of budget").
# This gate therefore uses the pinned program's OWN SPEC §5.1 smoke knob
# (the literal extra argument "smoke", N_BASE=2000 — bigger than the
# bench-local N=400 cells the prior gates used):
#  - arm "pinned-smoke": 10x  jsc <GIL-off flags> bench.js -- 16 smoke   (JIT-on)
#  - arm "nojit-smoke":   4x  same + --useJIT=0  (the A6 face's original profile)
#  - arm "fullsize":      1x  bench.js -- 16 (no smoke), timeout 7200, JIT-on,
#    started first and reported whenever it resolves.
# Waves of 3 concurrent smokes (+ the fullsize). EVERY log archived; per-run
# rc summary lines are evidentiary.
set -u
cd /root/WebKit
OUT=/root/WebKit/Tools/threads/bughunt/w16/rounds/a6amend-asan
mkdir -p "$OUT"
SUM="$OUT/SUMMARY.txt"
: > "$SUM"
GILOFF="--useJSThreads=1 --useThreadGIL=0 --useVMLite=1 --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1"
export ASAN_OPTIONS="abort_on_error=0:fast_unwind_on_malloc=0:malloc_context_size=30:detect_leaks=0:handle_segv=1:handle_abort=1:exitcode=99"
ulimit -c 0
{ echo "host precondition:"; uptime; pgrep -af "Limp" | grep -v $$ || echo "  no foreign Limp jsc"; } >> "$SUM"
run_one() {
  local label=$1 i=$2 extra=$3 sizearg=$4 tmo=$5
  local log="$OUT/${label}-${i}.log"
  /usr/bin/time -f "wall=%es maxrss=%MkB" timeout "$tmo" WebKitBuild/Debug/bin/jsc $GILOFF $extra Tools/threads/scalebench/js/bench.js -- 16 $sizearg > "$log" 2>&1
  local rc=$?
  local cls="FAIL"
  [ "$rc" = "0" ] && cls="PASS"
  [ "$rc" = "124" ] && cls="TIMEOUT"
  echo "${label}-${i} rc=$rc bytes=$(stat -c%s "$log") $cls" >> "$SUM"
}
run_one fullsize 1 "" "" 7200 &
FULLPID=$!
active=0
for q in pinned-smoke:1: pinned-smoke:2: pinned-smoke:3: pinned-smoke:4: pinned-smoke:5: pinned-smoke:6: pinned-smoke:7: pinned-smoke:8: pinned-smoke:9: pinned-smoke:10: nojit-smoke:1:--useJIT=0 nojit-smoke:2:--useJIT=0 nojit-smoke:3:--useJIT=0 nojit-smoke:4:--useJIT=0; do
  IFS=: read -r label i extra <<< "$q"
  run_one "$label" "$i" "$extra" smoke 2400 &
  active=$((active+1))
  if [ "$active" -ge 3 ]; then wait -n; active=$((active-1)); fi
done
# Drain the smoke arms (the fullsize attempt may still be running).
while [ "$(jobs -rp | grep -cv "^${FULLPID}$")" -gt 0 ]; do
  for p in $(jobs -rp); do [ "$p" != "$FULLPID" ] && wait "$p"; done
done
echo "A6AMEND-ASAN-SMOKE-DONE" >> "$SUM"
wait "$FULLPID" 2>/dev/null
echo "A6AMEND-ASAN-DONE" >> "$SUM"
