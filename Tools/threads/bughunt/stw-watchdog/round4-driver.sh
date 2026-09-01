#!/usr/bin/env bash
# round4-driver.sh — EXPERIMENTER round 4: all legs, sequential, on the stock
# landed-fix jsc, under the WD-4 gate (soak4.sh).
set -u
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JSC=/root/WebKit/WebKitBuild/Release/bin/jsc
FLAGS=(--useJSThreads=1 --useThreadGIL=0 --useVMLite=1 --useSharedAtomStringTable=1 --useSharedGCHeap=1 --useThreadGILOffUnsafe=1)
SC=/root/WebKit/JSTests/threads/scaling

echo "=== leg 1: ARB synthetic lock-free fires x50 (hang gate 60s) ==="
"$DIR/soak4.sh" 50 r4arb 60 -- "$JSC" "${FLAGS[@]}" \
    -e "globalThis.ARB_BATCHES=800;" "$DIR/experiments/arb-lockfree-fires.js"

echo "=== leg 2: richards-like N=4 scale=1/16 noFTL x100 (hang gate 60s) ==="
"$DIR/soak4.sh" 100 r4rich 60 -- "$JSC" "${FLAGS[@]}" --useFTLJIT=0 \
    -e "globalThis.SCALING_THREADS=4; globalThis.SCALING_WORK_SCALE=0.0625;" \
    "$SC/richards-like.js"

echo "=== leg 3: repro.js N=4 x100 (hang gate 60s) ==="
"$DIR/soak4.sh" 100 r4repro 60 -- "$JSC" "${FLAGS[@]}" "$DIR/../repro.js"

echo "=== leg 4 (TT3): repro.js N=4, DFG+FTL on, tiny tier-up threshold x50 (hang gate 90s) ==="
"$DIR/soak4.sh" 50 r4tt3 90 -- "$JSC" "${FLAGS[@]}" \
    --useDFGJIT=1 --useFTLJIT=1 --thresholdForOptimizeAfterWarmUp=10 \
    "$DIR/../repro.js"

echo "=== leg 5: string-heavy N=8 scale=1 x100 (hang gate 150s, mid-run gdb sample on first 3) ==="
SAMPLE_AT=15 "$DIR/soak4.sh" 100 r4sh8 150 --sample-first 3 -- "$JSC" "${FLAGS[@]}" \
    -e "globalThis.SCALING_THREADS=8; globalThis.SCALING_WORK_SCALE=1;" \
    "$SC/string-heavy.js"

echo "=== round 4 driver done ==="
