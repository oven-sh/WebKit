#!/bin/bash
# ASAN Debug sweep for residual W>=16 crash family.
# Usage: sweep.sh <W> <rounds> <smoke:0|1> <outdir-prefix>
set -u
W=$1; ROUNDS=$2; SMOKE=$3; PREFIX=$4
EXTRA="${5:-}"
cd /root/WebKit
OUT=/root/WebKit/Tools/threads/bughunt/w16/$PREFIX
mkdir -p "$OUT"
BENCH=/root/WebKit/Tools/threads/bughunt/w16/bench-local.js
FLAGS="--useJSThreads=1 --useThreadGIL=0 --useVMLite=1 --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1 $EXTRA"
export ASAN_OPTIONS="abort_on_error=0:fast_unwind_on_malloc=0:malloc_context_size=30:detect_leaks=0:handle_segv=1:handle_sigbus=1:handle_abort=1:handle_sigfpe=1:exitcode=99"
for i in $(seq 1 "$ROUNDS"); do
  LOG="$OUT/run-W${W}-$i.log"
  if [ "$SMOKE" = "1" ]; then
    # NOTE: jsc cannot read /proc/self/environ; the literal "smoke" arg is required.
    timeout 600 WebKitBuild/Debug/bin/jsc $FLAGS "$BENCH" -- "$W" smoke > "$LOG" 2>&1
  else
    timeout 600 WebKitBuild/Debug/bin/jsc $FLAGS "$BENCH" -- "$W" > "$LOG" 2>&1
  fi
  rc=$?
  echo "RC=$rc" >> "$LOG"
  if [ "$rc" = "0" ]; then
    rm -f "$LOG"
    echo "run $i W=$W: PASS"
  else
    echo "run $i W=$W: FAIL rc=$rc -> $LOG"
  fi
done
