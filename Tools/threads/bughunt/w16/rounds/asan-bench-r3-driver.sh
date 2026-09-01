#!/bin/bash
# asan-bench round 3 sweep (collector; READ-ONLY on Source).
# Gate of record per asan-bench-r2 DISPOSITION: ASAN Debug jsc, pinned GIL-off
# flags, bench.js -- 16 smoke x5 and -- 32 smoke x3, timeout >= 720 s.
set -u
cd /root/WebKit
OUT=/root/WebKit/Tools/threads/bughunt/w16/rounds/asan-bench-r3
mkdir -p "$OUT"
BENCH=/root/WebKit/Tools/threads/scalebench/bench.js
JSC=/root/WebKit/WebKitBuild/Debug/bin/jsc
FLAGS="--useJSThreads=1 --useThreadGIL=0 --useVMLite=1 --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1"
export ASAN_OPTIONS="abort_on_error=0:fast_unwind_on_malloc=0:malloc_context_size=30:detect_leaks=0:handle_segv=1:handle_sigbus=1:handle_abort=1:handle_sigfpe=1:exitcode=99"
ulimit -c unlimited

run_one() {
  local w=$1 i=$2
  local log="$OUT/W${w}-smoke-${i}.log"
  local t0=$(date +%s)
  timeout 1800 "$JSC" $FLAGS "$BENCH" -- "$w" smoke > "$log" 2>&1
  local rc=$?
  local t1=$(date +%s)
  echo "RC=$rc" >> "$log"
  local result=$(grep -m1 '"impl"' "$log" 2>/dev/null || echo "no-result-line")
  local suspects=$(grep -c -E "MISMATCH|AddressSanitizer|pas panic|ASSERT FAILED" "$log" 2>/dev/null)
  echo "$(date -u +%FT%TZ) W=$w smoke run=$i rc=$rc wall=$((t1-t0))s suspect_lines=${suspects:-0} result=$result" >> "$OUT/RECEIPTS.txt"
  if [ "$rc" = "0" ] && [ "${suspects:-0}" = "0" ]; then
    rm -f "$log"
    echo "W=$w run=$i PASS wall=$((t1-t0))s"
  else
    echo "W=$w run=$i FAIL rc=$rc -> $log"
  fi
}

echo "round3 start $(date -u +%FT%TZ)" >> "$OUT/RECEIPTS.txt"
# Batch A: W16 x3 parallel (48 threads)
run_one 16 1 & run_one 16 2 & run_one 16 3 & wait
# Batch B: W16 x2 parallel
run_one 16 4 & run_one 16 5 & wait
# Batch C: W32 x2 parallel (64 threads)
run_one 32 1 & run_one 32 2 & wait
# Batch D: W32 x1
run_one 32 3
echo "round3 done $(date -u +%FT%TZ)" >> "$OUT/RECEIPTS.txt"
touch "$OUT/DONE"
