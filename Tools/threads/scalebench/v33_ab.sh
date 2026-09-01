#!/usr/bin/env bash
# Run 3.3 A/B: d8ed7b6f5254 baseline vs campaign-3 v33, same host, back-to-back per cell.
set -u
SB="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE="$SB/../../../WebKitBuild/Release/bin/jsc-v33-baseline"
V33="$SB/../../../WebKitBuild/Release/bin/jsc"
PINNED="JSC_useJSThreads=1 JSC_useThreadGIL=0 JSC_useVMLite=1 JSC_useSharedAtomStringTable=1 JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1"
RAW="$SB/results-v33ab-raw.jsonl"
ulimit -c 0

run1() {
    local label=$1 bin=$2 W=$3 envv=$4 rep=$5
    local out tv
    tv=$(mktemp)
    out=$(env $envv /usr/bin/time -v -o "$tv" "$bin" "$SB/js/bench.js" -- "$W" 2>&1)
    local rc=$?
    local rss=$(grep 'Maximum resident' "$tv" | awk '{print $NF}')
    local user=$(grep 'User time' "$tv" | awk '{print $NF}')
    local syss=$(grep 'System time' "$tv" | awk '{print $NF}')
    local elap=$(grep 'Elapsed (wall' "$tv" | sed 's/.*): //')
    rm -f "$tv"
    local json=$(echo "$out" | grep '"impl"' | tail -1)
    local total=$(echo "$json" | python3 -c 'import sys,json; print(json.load(sys.stdin)["total_ms"])' 2>/dev/null || echo null)
    local cs=$(echo "$json" | python3 -c 'import sys,json; j=json.load(sys.stdin); print("|".join(str(j[k]) for k in ["checksumA","postings","checksumA2","checksumB","checksumC"]))' 2>/dev/null || echo null)
    echo "{\"label\":\"$label\",\"bin\":\"$(basename $bin)\",\"W\":$W,\"rep\":$rep,\"rc\":$rc,\"total_ms\":$total,\"rss_kb\":$rss,\"user_s\":$user,\"sys_s\":$syss,\"elapsed\":\"$elap\",\"cs\":\"$cs\",\"load\":\"$(awk '{print $1}' /proc/loadavg)\"}" | tee -a "$RAW"
}

cell() {
    local label=$1 W=$2 envv=$3 reps=$4
    echo "=== $label W=$W (loadavg $(awk '{print $1}' /proc/loadavg)) ===" >&2
    for rep in $(seq 1 $reps); do run1 "$label-base" "$BASE" "$W" "$envv" "$rep"; done
    for rep in $(seq 1 $reps); do run1 "$label-v33" "$V33" "$W" "$envv" "$rep"; done
}

cell_interleaved() {
    local label=$1 W=$2 envv=$3 reps=$4
    echo "=== $label W=$W INTERLEAVED (loadavg $(awk '{print $1}' /proc/loadavg)) ===" >&2
    for rep in $(seq 1 $reps); do
        run1 "$label-base" "$BASE" "$W" "$envv" "$rep"
        run1 "$label-v33" "$V33" "$W" "$envv" "$rep"
    done
}

case "${1:-all}" in
    w1) cell giloff-w1 1 "$PINNED" 3 ;;
    w2) cell giloff-w2 2 "$PINNED" 3 ;;
    w4) cell giloff-w4 4 "$PINNED" 3 ;;
    w8) cell giloff-w8 8 "$PINNED" 3 ;;
    w16) cell giloff-w16 16 "$PINNED" 3 ;;
    w32) cell giloff-w32 32 "$PINNED" 3 ;;
    gilon) cell_interleaved gilon-w1 1 "JSC_useJSThreads=1" 5 ;;
    congc)
        echo "=== congc W=4 v33 only (loadavg $(awk '{print $1}' /proc/loadavg)) ===" >&2
        for rep in 1 2 3 4 5; do run1 "congc-w4-v33" "$V33" 4 "$PINNED JSC_useConcurrentSharedGCMarking=1" "$rep"; done
        ;;
    all)
        cell giloff-w1 1 "$PINNED" 3
        cell giloff-w2 2 "$PINNED" 3
        cell giloff-w4 4 "$PINNED" 3
        cell giloff-w8 8 "$PINNED" 3
        cell giloff-w16 16 "$PINNED" 3
        cell giloff-w32 32 "$PINNED" 3
        cell_interleaved gilon-w1 1 "JSC_useJSThreads=1" 5
        for rep in 1 2 3 4 5; do run1 "congc-w4-v33" "$V33" 4 "$PINNED JSC_useConcurrentSharedGCMarking=1" "$rep"; done
        ;;
esac
