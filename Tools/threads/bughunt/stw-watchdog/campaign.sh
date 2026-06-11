#!/usr/bin/env bash
# campaign.sh — STW-watchdog abort reproduction campaign.
# Usage: JSC=/path/to/jsc ./campaign.sh <runs> <nthreads> <workload.js> <tag> [extra jsc flags...]
# Logs each run to logs/<tag>-run<i>.log; summary line per run on stdout.
set -u
JSC="${JSC:-/root/WebKit/WebKitBuild/Release/bin/jsc}"
RUNS="$1"; N="$2"; SCRIPT="$3"; TAG="$4"; shift 4
EXTRA=("$@")
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$DIR/logs"
FLAGS=(--useJSThreads=1 --useThreadGIL=0 --useVMLite=1 --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1)
SCALE="${SCALE:-1}"
TIMEOUT="${RUN_TIMEOUT:-150}"
pass=0; abort=0; segv=0; tmo=0; other=0
for i in $(seq 1 "$RUNS"); do
    LOG="$DIR/logs/$TAG-run$i.log"
    start=$(date +%s.%N)
    timeout -k 10 "$TIMEOUT" "$JSC" "${FLAGS[@]}" "${EXTRA[@]}" \
        -e "globalThis.SCALING_THREADS=$N; globalThis.SCALING_WORK_SCALE=$SCALE;" \
        "$SCRIPT" >"$LOG" 2>&1
    rc=$?
    end=$(date +%s.%N)
    dur=$(echo "$end $start" | awk '{printf "%.1f", $1-$2}')
    verdict=other
    if [ $rc -eq 0 ]; then verdict=PASS; pass=$((pass+1));
    elif [ $rc -eq 134 ]; then
        if grep -q "failed to reach a stopped world" "$LOG"; then verdict=WATCHDOG-ABORT; else verdict=OTHER-ABORT; fi
        abort=$((abort+1));
    elif [ $rc -eq 139 ]; then verdict=SIGSEGV; segv=$((segv+1));
    elif [ $rc -eq 124 ] || [ $rc -eq 137 ]; then verdict=TIMEOUT; tmo=$((tmo+1));
    else other=$((other+1)); fi
    echo "run $i: rc=$rc ${dur}s $verdict"
done
echo "SUMMARY tag=$TAG N=$N scale=$SCALE runs=$RUNS pass=$pass abort134=$abort segv=$segv timeout=$tmo other=$other"
