#!/bin/bash
# Mirror run: every *.js file of a test directory through mirror.js (two JS
# threads executing the file's top-level code on one global object), GIL off.
#
#   run-mirror.sh <jsc> <outdir> <eval|func> [test dir] [parallel] [timeout secs]
#
# Environment: MIRROR_THREADS (default 2), MIRROR_JSC_OPTIONS (extra engine
# options, e.g. eager-compilation thresholds), TSAN_OPTIONS / ASAN_OPTIONS as the
# build needs. The GIL-off environment is set here.
#
# Output, under <outdir>:
#   all.txt        "file rc seconds" per test
#   findings.txt   tests whose status is neither 0 (ran) nor 3 (threw) nor 124
#                  (deadline hit while still burning CPU: the two threads drove
#                  the test's own logic into a loop, which is expected noise)
#   <file>.out     for each finding: the sanitizer/assertion excerpt or the tail
#                  of the output; for status 125 (deadline hit while blocked: a
#                  deadlock candidate) the backtraces of every thread.
set -u
JSC=$1; OUT=$2; MODE=$3
DIR=${4:-$(cd "$(dirname "$0")/../../../JSTests/stress" && pwd)}
PAR=${5:-32}; TMO=${6:-300}
HARNESS=$(cd "$(dirname "$0")" && pwd)/mirror.js
mkdir -p "$OUT/rc"
export JSC OUT MODE TMO HARNESS
export MIRROR_THREADS=${MIRROR_THREADS:-2}
export MIRROR_JSC_OPTIONS=${MIRROR_JSC_OPTIONS:-}
export JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1 JSC_useThreadGIL=0
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_stack_use_after_return=0:detect_leaks=0}
cd "$DIR"
ls *.js | xargs -P "$PAR" -I{} bash -c '
  t={}
  start=$(date +%s)
  "$JSC" --useJSThreads=1 --useDollarVM=1 --validateOptions=0 $MIRROR_JSC_OPTIONS "$HARNESS" -- "$MODE" "$t" "$MIRROR_THREADS" > "$OUT/rc/$t.log" 2>&1 &
  pid=$!
  rc=""
  while [ $(( $(date +%s) - start )) -lt "$TMO" ]; do
    if ! kill -0 $pid 2>/dev/null; then wait $pid; rc=$?; break; fi
    sleep 1
  done
  if [ -z "$rc" ]; then
    # Deadline. Blocked (no CPU over 3 s) or spinning?
    c0=$(awk "{print \$14+\$15}" /proc/$pid/stat 2>/dev/null || echo 0)
    sleep 3
    c1=$(awk "{print \$14+\$15}" /proc/$pid/stat 2>/dev/null || echo 0)
    if [ $(( c1 - c0 )) -lt 20 ]; then
      rc=125
      if command -v gdb >/dev/null; then
        gdb -q -batch -p $pid -ex "thread apply all bt 25" > "$OUT/rc/$t.bt" 2>/dev/null
      fi
    else
      rc=124
    fi
    kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null
  fi
  secs=$(( $(date +%s) - start ))
  echo "$t $rc $secs" > "$OUT/rc/$t.rc"
  if [ $rc -ne 0 ] && [ $rc -ne 3 ] && [ $rc -ne 124 ]; then
    {
      grep -v "disabling useWasm" "$OUT/rc/$t.log" | grep -B2 -A40 -m1 -E "ASSERTION FAILED|SHOULD NEVER BE REACHED|ERROR: AddressSanitizer|WARNING: ThreadSanitizer|Segmentation|signal" \
        || grep -v "disabling useWasm" "$OUT/rc/$t.log" | tail -40
      [ -f "$OUT/rc/$t.bt" ] && grep -E "^Thread|^#" "$OUT/rc/$t.bt"
    } > "$OUT/$t.out"
  else
    rm -f "$OUT/rc/$t.log"
  fi
'
cat "$OUT"/rc/*.rc | sort > "$OUT/all.txt"
awk '$2!=0 && $2!=3 && $2!=124 {print $1" rc="$2" "$3"s"}' "$OUT/all.txt" > "$OUT/findings.txt"
awk '$2==124 {print $1}' "$OUT/all.txt" > "$OUT/spinning-timeouts.txt"
echo "DONE $(date)" >> "$OUT/all.txt"
echo "files: $(ls "$OUT"/rc/*.rc | wc -l)  findings: $(wc -l < "$OUT/findings.txt")  spinning timeouts: $(wc -l < "$OUT/spinning-timeouts.txt")"
