#!/usr/bin/env bash
# amend3-campaign.sh — signature re-verification for the A4 amend round 3:
#   (1) varargsSetup snapshot pinned via relaxed atomic loads (CSE finding),
#   (2) same-VM guards extended to the JIT emission-side Group-3 readers
#       (op_catch, DFG OSR-exit exception ramp, jumpToExceptionHandler),
#   (3) targeted JIT-on GIL-off host-call hammer (review finding: the thunk
#       arm had no executed JIT-on gate).
# Legs (fast signal first):
#   leg D: 10x Release hostcall-hammer, forced tier-up + FreeList validator
#   leg E:  5x Debug ASAN hostcall-hammer, forced tier-up (JIT ON)
#   leg C: 10x Release full bench.js -- 16   (residual W>=16 crash-family gate)
#   leg A: 10x ASAN (Debug) minimal repro, --useJIT=0, W=4 (precise A1 signature)
#   leg B: 10x ASAN (Debug) bench.js -- 16 smoke (JIT ON)
#   leg F: flag-off smoke, both builds, NO thread flags
set -u
cd /root/WebKit
D=Tools/threads/bughunt/varargs-fix/amend3-logs
mkdir -p "$D"
FLAGS=(--useJSThreads=1 --useThreadGIL=0 --useVMLite=1 --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1)
TIER=(--thresholdForJITAfterWarmUp=10 --thresholdForOptimizeAfterWarmUp=100 --validateFreeListStructure=1)
R="$D/RESULTS.txt"

run_leg() { # leg runs timeout jsc extra... -- script args...
  local leg=$1 runs=$2 to=$3 jsc=$4; shift 4
  local pass=0 fail=0
  for i in $(seq 1 "$runs"); do
    local LOG="$D/$leg-run$i.log" start end rc
    start=$(date +%s)
    timeout -k 10 "$to" "$jsc" "$@" >"$LOG" 2>&1
    rc=$?
    end=$(date +%s)
    if [ $rc -eq 0 ] && ! grep -q "MISMATCH\|ERROR: AddressSanitizer" "$LOG"; then
      pass=$((pass+1)); echo "$leg run $i: rc=$rc $((end-start))s PASS" >> "$R"
    else
      fail=$((fail+1)); echo "$leg run $i: rc=$rc $((end-start))s FAIL" >> "$R"
    fi
  done
  echo "LEGSUMMARY $leg pass=$pass fail=$fail" >> "$R"
}

run_leg hammerD 10 300 WebKitBuild/Release/bin/jsc "${FLAGS[@]}" "${TIER[@]}" Tools/threads/bughunt/varargs-fix/hostcall-hammer.js
run_leg hammerE 5 600 WebKitBuild/Debug/bin/jsc "${FLAGS[@]}" "${TIER[@]}" -e "globalThis.HAMMER_ITERS=15000;" Tools/threads/bughunt/varargs-fix/hostcall-hammer.js
run_leg flagoffF 2 300 WebKitBuild/Release/bin/jsc Tools/threads/scalebench/js/bench.js -- 4 smoke
run_leg releaseC 10 300 WebKitBuild/Release/bin/jsc "${FLAGS[@]}" Tools/threads/scalebench/js/bench.js -- 16
run_leg reproA 10 300 WebKitBuild/Debug/bin/jsc "${FLAGS[@]}" --useJIT=0 -e "globalThis.REPRO_THREADS=4; globalThis.REPRO_ITERS=500000;" Tools/threads/bughunt/repro.js
run_leg smokeB 10 880 WebKitBuild/Debug/bin/jsc "${FLAGS[@]}" Tools/threads/scalebench/js/bench.js -- 16 smoke
echo "SUMMARY done" >> "$R"
