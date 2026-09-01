#!/usr/bin/env bash
# load6x.sh — bughunt variant of Tools/threads/load6.sh that accepts EXTRA_FLAGS
# (tier/stress narrowing) and never requires the PASS marker logic to change.
# Usage: EXTRA_FLAGS="--useFTLJIT=0" OUT=/tmp/x SEED_BASE=1000 RUNS_PER_WORKER=20 load6x.sh [test.js]
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
JSC=${JSC:-"$ROOT_DIR/WebKitBuild/Debug/bin/jsc"}
TEST=${1:-"$ROOT_DIR/JSTests/threads/jit/spawned-thread-butterfly-stress.js"}
OUT=${OUT:-/tmp/load6x-logs}
RUNS_PER_WORKER=${RUNS_PER_WORKER:-20}
SEED_BASE=${SEED_BASE:-1000}
TIMEOUT_S=${TIMEOUT_S:-300}
EXTRA_FLAGS=${EXTRA_FLAGS:-}
WORKERS=${WORKERS:-6}
mkdir -p "$OUT"
FLAGS=(--useJSThreads=1 --useThreadGIL=0 --useVMLite=1
       --useSharedAtomStringTable=1 --useSharedGCHeap=1
       --useThreadGILOffUnsafe=1 --useDollarVM=1)
# shellcheck disable=SC2206
EXTRA=($EXTRA_FLAGS)
worker() {
    local w=$1 i seed log rc
    for ((i = 0; i < RUNS_PER_WORKER; ++i)); do
        seed=$((SEED_BASE + w * 1000 + i))
        log="$OUT/w${w}-r${i}-s${seed}.log"
        timeout -k 5 "$TIMEOUT_S" "$JSC" "${FLAGS[@]}" "${EXTRA[@]+"${EXTRA[@]}"}" \
            --randomYieldPeriod=64 --randomYieldSeed=$seed "$TEST" >"$log" 2>&1
        rc=$?
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
failures=$(ls "$OUT" 2>/dev/null | grep -c "FAIL\|BAD")
echo "load6x[$EXTRA_FLAGS]: $((RUNS_PER_WORKER * WORKERS)) runs, $failures failures (logs in $OUT)"
exit 0
