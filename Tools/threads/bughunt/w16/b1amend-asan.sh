#!/bin/bash
# B1 amend round: re-verify the B1 signature 10x under ASAN Debug after the
# ctor reorder (m_previousScope deref moved out of the member-init list so
# the push-side window-coherence check + ASAN poison probe are the FIRST
# consumption of the chain anchor; see SIGNATURES.md 2026-06-11g AMEND).
# Profile = B1's exact face profile: pinned program smoke knob, GIL-off
# flags, --useJIT=0. Run r1 is executed separately for timing; this script
# runs r2..r10 in waves of 3 concurrent.
set -u
cd /root/WebKit
OUT=/tmp/b1amend
mkdir -p "$OUT"
SUM="$OUT/SUMMARY.txt"
GILOFF="--useJSThreads=1 --useThreadGIL=0 --useVMLite=1 --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1"
export ASAN_OPTIONS="abort_on_error=0:fast_unwind_on_malloc=0:malloc_context_size=30:detect_leaks=0:handle_segv=1:handle_abort=1:exitcode=99"
ulimit -c 0
run_one() {
  local i=$1
  local log="$OUT/r${i}.log"
  /usr/bin/time -f "wall=%es maxrss=%MkB" timeout 2700 WebKitBuild/Debug/bin/jsc $GILOFF --useJIT=0 Tools/threads/scalebench/js/bench.js -- 16 smoke > "$log" 2>&1
  local rc=$?
  local cls="FAIL"
  [ "$rc" = "0" ] && cls="PASS"
  [ "$rc" = "124" ] && cls="TIMEOUT"
  echo "r${i} rc=$rc bytes=$(stat -c%s "$log") $cls" >> "$SUM"
}
for wave in "2 3 4" "5 6 7" "8 9 10"; do
  for i in $wave; do run_one "$i" & done
  wait
done
echo "=== face scan ===" >> "$SUM"
grep -l "B1-DIAG\|verificationState == m_verificationStateAtConstruction\|AddressSanitizer" "$OUT"/r*.log >> "$SUM" 2>&1 || echo "no B1/ASAN faces in any log" >> "$SUM"
