#!/usr/bin/env bash
# bench-campaign.sh — scalebench bench.js campaign for the varargs fix verify.
# Usage: ./bench-campaign.sh <runs> <W> <tag> [extra jsc flags...]
# Runs bench.js in smoke mode; pass = rc 0 AND identical checksum JSON across runs.
set -u
JSC="${JSC:-/root/WebKit/WebKitBuild/Release/bin/jsc}"
RUNS="$1"; W="$2"; TAG="$3"; shift 3
EXTRA=("$@")
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_DIR="$DIR/../../scalebench"
mkdir -p "$DIR/logs"
FLAGS=(--useJSThreads=1 --useThreadGIL=0 --useVMLite=1 --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1)
TIMEOUT="${RUN_TIMEOUT:-300}"
pass=0; fail=0; ref_checksums=""
cd "$BENCH_DIR" || exit 2
for i in $(seq 1 "$RUNS"); do
    LOG="$DIR/logs/$TAG-run$i.log"
    SCALEBENCH_SMOKE=1 timeout -k 10 "$TIMEOUT" "$JSC" "${FLAGS[@]}" "${EXTRA[@]}" bench.js -- "$W" smoke >"$LOG" 2>&1
    rc=$?
    sums="$(grep -o '"checksum[^,]*' "$LOG" | tr '\n' ' ')"
    verdict="FAIL(rc=$rc)"
    if [ $rc -eq 0 ] && [ -n "$sums" ]; then
        if [ -z "$ref_checksums" ]; then ref_checksums="$sums"; fi
        if [ "$sums" == "$ref_checksums" ]; then
            verdict=PASS; pass=$((pass+1)); rm -f "$LOG"
        else
            verdict="FAIL(checksum-mismatch)"; fail=$((fail+1))
        fi
    else
        fail=$((fail+1))
    fi
    echo "run $i: rc=$rc $verdict"
done
echo "SUMMARY tag=$TAG W=$W runs=$RUNS pass=$pass fail=$fail checksums=$ref_checksums"
[ $fail -eq 0 ]
