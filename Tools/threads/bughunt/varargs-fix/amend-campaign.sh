#!/usr/bin/env bash
# amend-campaign.sh — signature re-verification for the host-call-return-value
# reader/writer symmetry amendment (same-VM guard in the getHostCallReturnValue
# JIT thunk + llint asm op), matching the EVIDENCE.md protocols:
#   leg A: 10x ASAN (Debug) minimal repro, --useJIT=0, W=4  (the precise A1 signature)
#   leg B: 10x ASAN (Debug) bench.js -- 16 smoke            (bench-level ASAN rate game)
#   leg C: 10x Release full bench.js -- 16                  (residual W>=16 crash-family gate)
set -u
cd /root/WebKit
D=Tools/threads/bughunt/varargs-fix/amend-logs
mkdir -p "$D"
FLAGS=(--useJSThreads=1 --useThreadGIL=0 --useVMLite=1 --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1)
R="$D/RESULTS.txt"

run_leg() { # leg runs timeout jsc extra... -- script args...
  local leg=$1 runs=$2 to=$3 jsc=$4; shift 4
  local pass=0 fail=0
  for i in $(seq 1 "$runs"); do
    local LOG="$D/$leg-run$i.log" start end rc
    start=$(date +%s)
    timeout -k 10 "$to" "$jsc" "${FLAGS[@]}" "$@" >"$LOG" 2>&1
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

# REPRO_ITERS calibrated for Debug ASAN (~3.7k iters/s at W=4: 500k ~= 135s);
# the Release-default 2M does not fit any sane timeout under ASAN no-JIT.
run_leg reproA 10 300 WebKitBuild/Debug/bin/jsc --useJIT=0 -e "globalThis.REPRO_THREADS=4; globalThis.REPRO_ITERS=500000;" Tools/threads/bughunt/repro.js
run_leg releaseC 10 300 WebKitBuild/Release/bin/jsc Tools/threads/scalebench/js/bench.js -- 16
run_leg smokeB 10 880 WebKitBuild/Debug/bin/jsc Tools/threads/scalebench/js/bench.js -- 16 smoke
echo "SUMMARY done" >> "$R"
