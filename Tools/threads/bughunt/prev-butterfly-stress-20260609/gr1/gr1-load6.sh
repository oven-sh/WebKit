#!/usr/bin/env bash
# gr1-load6.sh — load6 variant for the GR1 experiment: before deleting a
# passing log, extract all [BUGHUNT-GR1] lines into $OUT/gr1-extract.log
# (prefixed with the run id). Failure logs are kept whole as usual.
set -u
ROOT_DIR=/root/WebKit
JSC=${JSC:-"$ROOT_DIR/WebKitBuild/Debug/bin/jsc"}
TEST=${1:-"$ROOT_DIR/JSTests/threads/jit/spawned-thread-butterfly-stress.js"}
OUT=${OUT:-/tmp/gr1-logs}
RUNS_PER_WORKER=${RUNS_PER_WORKER:-20}
SEED_BASE=${SEED_BASE:-1000}
TIMEOUT_S=${TIMEOUT_S:-240}
WORKERS=${WORKERS:-6}
mkdir -p "$OUT"
EXTRACT="$OUT/gr1-extract.log"
: > "$EXTRACT"
FLAGS=(--useJSThreads=1 --useThreadGIL=0 --useVMLite=1
       --useSharedAtomStringTable=1 --useSharedGCHeap=1
       --useThreadGILOffUnsafe=1 --useDollarVM=1 --destroy-vm)
worker() {
    local w=$1 i seed log rc
    for ((i = 0; i < RUNS_PER_WORKER; ++i)); do
        seed=$((SEED_BASE + w * 1000 + i))
        log="$OUT/w${w}-r${i}-s${seed}.log"
        timeout -k 5 "$TIMEOUT_S" "$JSC" "${FLAGS[@]}" ${EXTRA_FLAGS:-} \
            --randomYieldPeriod=64 --randomYieldSeed=$seed "$TEST" >"$log" 2>&1
        rc=$?
        grep "BUGHUNT-GR1" "$log" | sed "s/^/[w${w}-r${i}-s${seed} rc=${rc}] /" >> "$EXTRACT"
        if [[ $rc -ne 0 ]]; then
            mv "$log" "$OUT/FAIL-rc${rc}-w${w}-r${i}-s${seed}.log"
        elif ! grep -q "PASS" "$log"; then
            mv "$log" "$OUT/BAD-w${w}-r${i}-s${seed}.log"
        else
            rm -f "$log"
        fi
    done
}
for ((w = 0; w < WORKERS; ++w)); do worker "$w" & done
wait
fails=$(ls "$OUT" | grep -c "FAIL\|BAD")
echo "gr1-load6: $((RUNS_PER_WORKER * WORKERS)) runs, $fails failures"
echo "gr1 events: overlaps=$(grep -c OVERLAP "$EXTRACT") anomalies=$(grep -c ANOMALY "$EXTRACT") shadow=$(grep -c SHADOW-MISMATCH "$EXTRACT") vehicle=$(grep -c CONCATKEY-VEHICLE "$EXTRACT")"
