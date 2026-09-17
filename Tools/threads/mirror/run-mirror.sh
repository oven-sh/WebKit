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
# Memory: a file whose header says `//@ memoryHog!` allocates until something
# stops it (run-jsc-stress-tests runs those alone, with the options of their
# `//@ run...` line - usually a watchdog). They run here in a second pass,
# MIRROR_HOG_PARALLEL at a time (default 2), with the `--option` arguments of
# their first `//@ run...(...)` / `//@ requireOptions(...)` line; MIRROR_SKIP_HOGS=1
# skips them instead. Every process, hog or not, is killed when its resident set
# passes MIRROR_RSS_LIMIT_MB (default 6144; 0 disables): status 126, reported in
# memory-capped.txt and not a finding.
#
# Output, under <outdir>:
#   all.txt        "file rc seconds" per test
#   findings.txt   tests whose status is neither 0 (ran) nor 3 (threw) nor 124
#                  (deadline hit while still burning CPU: the two threads drove
#                  the test's own logic into a loop, which is expected noise)
#                  nor 126 (resident-set cap)
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
export MIRROR_RSS_LIMIT_MB=${MIRROR_RSS_LIMIT_MB:-6144}
HOG_PAR=${MIRROR_HOG_PARALLEL:-2}
export JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1 JSC_useThreadGIL=0
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_stack_use_after_return=0:detect_leaks=0}
cd "$DIR"

runone='
  t=$1
  # The engine options a memory hog asks for: the quoted "--..." arguments of its
  # first //@ run...(...) or //@ requireOptions(...) line.
  hogopts=""
  if [ "${2:-}" = hog ]; then
    hogopts=$(grep -m1 -E "^//@ *(run[A-Za-z]*|requireOptions)\(" "$t" | grep -o "\"--[^\"]*\"" | tr -d "\"" | grep -v -- "--watchdog-exception-ok" | tr "\n" " ")
    # --watchdog-exception-ok is a run-jsc-stress-tests argument, not an engine
    # option; a watchdog termination exits 3 under the mirror, a non-finding.
  fi
  start=$(date +%s)
  "$JSC" --useJSThreads=1 --useDollarVM=1 --validateOptions=0 $MIRROR_JSC_OPTIONS $hogopts "$HARNESS" -- "$MODE" "$t" "$MIRROR_THREADS" > "$OUT/rc/$t.log" 2>&1 &
  pid=$!
  rc=""
  limitkb=$(( MIRROR_RSS_LIMIT_MB * 1024 ))
  ticks=0
  while [ $(( $(date +%s) - start )) -lt "$TMO" ]; do
    if ! kill -0 $pid 2>/dev/null; then wait $pid; rc=$?; break; fi
    if [ $limitkb -gt 0 ]; then
      rsskb=$(awk "/^VmRSS:/ {print \$2}" /proc/$pid/status 2>/dev/null)
      if [ -n "$rsskb" ] && [ "$rsskb" -gt $limitkb ]; then
        kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null
        rc=126
        echo "mirror: resident set ${rsskb} kB passed the ${MIRROR_RSS_LIMIT_MB} MB cap after $(( $(date +%s) - start )) s" >> "$OUT/rc/$t.log"
        break
      fi
    fi
    # Poll quickly at first (most files finish in well under a second), then four times a second:
    # a hog grows by up to a gigabyte a second.
    ticks=$(( ticks + 1 ))
    if [ $ticks -lt 20 ]; then sleep 0.05; else sleep 0.25; fi
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
  if [ $rc -ne 0 ] && [ $rc -ne 3 ] && [ $rc -ne 124 ] && [ $rc -ne 126 ]; then
    {
      grep -v "disabling useWasm" "$OUT/rc/$t.log" | grep -B2 -A40 -m1 -E "ASSERTION FAILED|SHOULD NEVER BE REACHED|ERROR: AddressSanitizer|WARNING: ThreadSanitizer|Segmentation|signal" \
        || grep -v "disabling useWasm" "$OUT/rc/$t.log" | tail -40
      [ -f "$OUT/rc/$t.bt" ] && grep -E "^Thread|^#" "$OUT/rc/$t.bt"
    } > "$OUT/$t.out"
  else
    rm -f "$OUT/rc/$t.log"
  fi
'
export runone

ls *.js > "$OUT/files.txt"
grep -l -E "^//@ *memoryHog!" *.js 2>/dev/null > "$OUT/memory-hogs.txt"
grep -v -x -F -f "$OUT/memory-hogs.txt" "$OUT/files.txt" > "$OUT/ordinary.txt"

xargs -a "$OUT/ordinary.txt" -P "$PAR" -I{} bash -c "$runone" _ {}
if [ "${MIRROR_SKIP_HOGS:-0}" = 1 ]; then
  echo "skipped $(wc -l < "$OUT/memory-hogs.txt") memory hogs (MIRROR_SKIP_HOGS=1)"
else
  xargs -a "$OUT/memory-hogs.txt" -P "$HOG_PAR" -I{} bash -c "$runone" _ {} hog
fi

cat "$OUT"/rc/*.rc | sort > "$OUT/all.txt"
awk '$2!=0 && $2!=3 && $2!=124 && $2!=126 {print $1" rc="$2" "$3"s"}' "$OUT/all.txt" > "$OUT/findings.txt"
awk '$2==124 {print $1}' "$OUT/all.txt" > "$OUT/spinning-timeouts.txt"
awk '$2==126 {print $1}' "$OUT/all.txt" > "$OUT/memory-capped.txt"
echo "DONE $(date)" >> "$OUT/all.txt"
echo "files: $(ls "$OUT"/rc/*.rc | wc -l)  findings: $(wc -l < "$OUT/findings.txt")  spinning timeouts: $(wc -l < "$OUT/spinning-timeouts.txt")  memory-capped: $(wc -l < "$OUT/memory-capped.txt")  memory hogs: $(wc -l < "$OUT/memory-hogs.txt")"
