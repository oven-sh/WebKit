#!/usr/bin/env bash
# Run 3.2 A/B: baseline (HEAD-only) vs v32, same host, back-to-back per cell.
set -u
SB="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE="$SB/../../../WebKitBuild/Release/bin/jsc-v32-baseline"
V32="$SB/../../../WebKitBuild/Release/bin/jsc"
PINNED="JSC_useJSThreads=1 JSC_useThreadGIL=0 JSC_useVMLite=1 JSC_useSharedAtomStringTable=1 JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1"
RAW="$SB/results-v32ab-raw.jsonl"
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
    echo "{\"label\":\"$label\",\"bin\":\"$(basename $bin)\",\"W\":$W,\"rep\":$rep,\"rc\":$rc,\"total_ms\":$total,\"rss_kb\":$rss,\"user_s\":$user,\"sys_s\":$syss,\"elapsed\":\"$elap\",\"cs\":\"$cs\"}" | tee -a "$RAW"
}

cell() {
    local label=$1 W=$2 envv=$3
    echo "=== $label W=$W (loadavg $(awk '{print $1}' /proc/loadavg)) ==="
    for rep in 1 2 3; do run1 "$label-base" "$BASE" "$W" "$envv" "$rep"; done
    for rep in 1 2 3; do run1 "$label-v32" "$V32" "$W" "$envv" "$rep"; done
}

cell giloff-w1 1 "$PINNED"
cell giloff-w2 2 "$PINNED"
cell giloff-w8 8 "$PINNED"
cell giloff-w16 16 "$PINNED"
cell giloff-w32 32 "$PINNED"
cell gilon-w1 1 "JSC_useJSThreads=1"
