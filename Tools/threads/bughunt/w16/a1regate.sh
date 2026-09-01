#!/usr/bin/env bash
# a1regate.sh — re-run of the A1 final gate with UNCONDITIONAL positive
# per-run records, against the FINAL tree binaries (Debug 22:37 / Release
# 22:40, ninja-verified no-work vs sources). Addresses the review findings
# on session 2026-06-11j:
#  - the claimed 21/21 ASAN wave gate left only empty directories;
#  - JIT-on sanitized coverage of the final tree was zero;
#  - the flag-off leg was a vacuous harness failure (bench.js needs Lock);
#  - hammer legs predated the final LLIntSlowPaths/Options/ExceptionScope/
#    Heap edits.
# Every run writes "<leg> run N: rc=<rc> <secs>s PASS|FAIL|HARNESS-FAIL"
# plus the last stdout line, into rounds/a1regate/RESULTS.txt. A run whose
# log shows ReferenceError/SyntaxError at startup is HARNESS-FAIL, which is
# counted as a campaign defect, never silently ignored.
set -u
cd /root/WebKit
OUT=Tools/threads/bughunt/w16/rounds/a1regate
mkdir -p "$OUT"
R="$OUT/RESULTS.txt"
FLAGS=(--useJSThreads=1 --useThreadGIL=0 --useVMLite=1 --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1)
TIER=(--thresholdForJITAfterWarmUp=10 --thresholdForOptimizeAfterWarmUp=100 --validateFreeListStructure=1)
export ASAN_OPTIONS="abort_on_error=0:fast_unwind_on_malloc=0:malloc_context_size=30:detect_leaks=0:handle_segv=1:handle_abort=1:exitcode=99"
ulimit -c 0

run_one() { # run_one <leg> <i> <timeout> <jsc> args...
  local leg=$1 i=$2 to=$3 jsc=$4; shift 4
  local LOG="$OUT/$leg-run$i.log" start end rc cls
  start=$(date +%s)
  timeout -k 10 "$to" "$jsc" "$@" > "$LOG" 2>&1
  rc=$?
  end=$(date +%s)
  cls=PASS
  if grep -qE "ReferenceError|SyntaxError|usage:" "$LOG"; then
    cls=HARNESS-FAIL
  elif [ "$rc" -ne 0 ] || grep -qE "MISMATCH|ERROR: AddressSanitizer|pas panic" "$LOG"; then
    cls=FAIL
  fi
  echo "$leg run $i: rc=$rc $((end-start))s $cls | $(tail -c 220 "$LOG" | tr '\n' ' ')" >> "$R"
  [ "$cls" = PASS ]
}
"$@"
