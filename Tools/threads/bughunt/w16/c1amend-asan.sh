#!/bin/bash
# C1 AMEND gate (session 2026-06-11h): 10x ASAN Debug, exact C1 repro cell —
# pinned GIL-off flags + --collectContinuously=1, bench-local.js -- 16 smoke 600,
# 720s timeout. ccgc makes the bench non-terminating within the budget, so
# PASS = rc 0 or 124 AND zero "ASSERTION FAILED" / ERROR: AddressSanitizer /
# SIGTRAP/SIGSEGV faces in the log (same acceptance as the landed C1 gate).
set -u
cd /root/WebKit
OUT=/root/WebKit/Tools/threads/bughunt/w16/rounds/c1amend
mkdir -p "$OUT"
BENCH=/root/WebKit/Tools/threads/bughunt/w16/bench-local.js
FLAGS="--useJSThreads=1 --useThreadGIL=0 --useVMLite=1 --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1 --collectContinuously=1"
export ASAN_OPTIONS="abort_on_error=0:fast_unwind_on_malloc=0:malloc_context_size=30:detect_leaks=0:handle_segv=1:handle_abort=1:exitcode=99"
ulimit -c 0
run_one() {
  local i=$1
  local log="$OUT/W16-N600-ccgc-$i.log"
  timeout 720 WebKitBuild/Debug/bin/jsc $FLAGS "$BENCH" -- 16 smoke 600 > "$log" 2>&1
  local rc=$?
  echo "RC=$rc" >> "$log"
  local bad=""
  grep -q "ASSERTION FAILED\|ERROR: AddressSanitizer\|SUMMARY: AddressSanitizer\|Fatal:\|RELEASE_ASSERT" "$log" && bad="face"
  if { [ "$rc" = "0" ] || [ "$rc" = "124" ]; } && [ -z "$bad" ]; then
    echo "[c1amend] #$i PASS rc=$rc"
  else
    echo "[c1amend] #$i FAIL rc=$rc $bad (log kept: $log)"
  fi
}
pids=()
for i in $(seq 1 "${RUNS:-10}"); do
  run_one "$i" & pids+=($!)
  # stagger waves of WAVE (default 3) concurrent runs
  if (( i % ${WAVE:-3} == 0 )); then for p in "${pids[@]}"; do wait "$p"; done; pids=(); fi
done
for p in "${pids[@]}"; do wait "$p"; done
echo "[c1amend] gate complete"
grep -c "" /dev/null >/dev/null
