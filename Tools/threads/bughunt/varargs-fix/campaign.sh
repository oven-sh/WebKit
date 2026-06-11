#!/usr/bin/env bash
# campaign.sh — H1 varargs shared-VM-scratch fix verification campaign.
# Usage: JSC=/path/to/jsc ./campaign.sh <runs> <nthreads> <workload.js> <tag> [extra jsc flags...]
# Logs each run to logs/<tag>-run<i>.log; summary line per run on stdout.
# Pass criterion: rc==0 AND no "MISMATCH" line in the log (repro.js prints
# silent-corruption decodes without dying on some faces).
set -u
JSC="${JSC:-/root/WebKit/WebKitBuild/Release/bin/jsc}"
RUNS="$1"; N="$2"; SCRIPT="$3"; TAG="$4"; shift 4
EXTRA=("$@")
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$DIR/logs"
FLAGS=(--useJSThreads=1 --useThreadGIL=0 --useVMLite=1 --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1)
TIMEOUT="${RUN_TIMEOUT:-150}"
ITERS="${REPRO_ITERS:-}"
pass=0; fail=0
for i in $(seq 1 "$RUNS"); do
    LOG="$DIR/logs/$TAG-run$i.log"
    start=$(date +%s.%N)
    PRELUDE="globalThis.REPRO_THREADS=$N; globalThis.SCALEBENCH_THREADS=$N;"
    [ -n "$ITERS" ] && PRELUDE="$PRELUDE globalThis.REPRO_ITERS=$ITERS;"
    timeout -k 10 "$TIMEOUT" "$JSC" "${FLAGS[@]}" "${EXTRA[@]}" \
        -e "$PRELUDE" "$SCRIPT" >"$LOG" 2>&1
    rc=$?
    end=$(date +%s.%N)
    dur=$(echo "$end $start" | awk '{printf "%.1f", $1-$2}')
    verdict=FAIL
    if [ $rc -eq 0 ] && ! grep -q "MISMATCH" "$LOG"; then
        verdict=PASS; pass=$((pass+1)); rm -f "$LOG"
    else
        fail=$((fail+1))
        [ $rc -ne 0 ] && verdict="FAIL(rc=$rc)" || verdict="FAIL(mismatch)"
    fi
    echo "run $i: rc=$rc ${dur}s $verdict"
done
echo "SUMMARY tag=$TAG N=$N runs=$RUNS pass=$pass fail=$fail"
[ $fail -eq 0 ]
