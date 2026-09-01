#!/usr/bin/env bash
# Run 3.9 A/B: d8ed7b6f5254 baseline (jsc-v33-baseline, sha 2a85f8e5…) vs v39
# (campaign-4 C1-reversal: useConcurrentSharedGCMarking forced on under
#  !useThreadGIL && useSharedGCHeap. Single Options.cpp delta over v38).
# BOTH arms: BigInt (spec-exact) and intcs (§29 fairness amendment).
set -u
SB="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE="$SB/../../../WebKitBuild/Release/bin/jsc-v33-baseline"
V39="$SB/../../../WebKitBuild/Release/bin/jsc-v39"
PINNED="JSC_useJSThreads=1 JSC_useThreadGIL=0 JSC_useVMLite=1 JSC_useSharedAtomStringTable=1 JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1"
RAW="$SB/results-v39ab-raw.jsonl"
ulimit -c 0

run1() {
    local label=$1 bin=$2 W=$3 envv=$4 rep=$5 arm=${6:-}
    local out tv
    tv=$(mktemp)
    out=$(env $envv /usr/bin/time -v -o "$tv" "$bin" "$SB/js/bench.js" -- "$W" $arm 2>&1)
    local rc=$?
    local rss=$(grep 'Maximum resident' "$tv" | awk '{print $NF}')
    local user=$(grep 'User time' "$tv" | awk '{print $NF}')
    local syss=$(grep 'System time' "$tv" | awk '{print $NF}')
    local elap=$(grep 'Elapsed (wall' "$tv" | sed 's/.*): //')
    rm -f "$tv"
    local json=$(echo "$out" | grep '"impl"' | tail -1)
    local total=$(echo "$json" | python3 -c 'import sys,json; print(json.load(sys.stdin)["total_ms"])' 2>/dev/null || echo null)
    local cs=$(echo "$json" | python3 -c 'import sys,json; j=json.load(sys.stdin); print("|".join(str(j[k]) for k in ["checksumA","postings","checksumA2","checksumB","checksumC"]))' 2>/dev/null || echo null)
    [[ -z "$json" ]] && json=null
    local errtail=""
    if [[ $rc -ne 0 ]]; then
        errtail=$(echo "$out" | grep -v '^JSC:' | tail -8 | tr '\n' '~' | sed 's/"/\\"/g')
    fi
    echo "{\"label\":\"$label\",\"bin\":\"$(basename $bin)\",\"W\":$W,\"arm\":\"${arm:-bigint}\",\"rep\":$rep,\"rc\":$rc,\"total_ms\":$total,\"rss_kb\":${rss:-null},\"user_s\":${user:-null},\"sys_s\":${syss:-null},\"elapsed\":\"$elap\",\"cs\":\"$cs\",\"load\":\"$(awk '{print $1}' /proc/loadavg)\",\"err\":\"$errtail\",\"json\":$json}" | tee -a "$RAW"
}

cell() {
    local label=$1 W=$2 envv=$3 reps=$4 arm=${5:-}
    echo "=== $label W=$W arm=${arm:-bigint} (loadavg $(awk '{print $1}' /proc/loadavg)) ===" >&2
    for rep in $(seq 1 $reps); do run1 "$label-base" "$BASE" "$W" "$envv" "$rep" "$arm"; done
    for rep in $(seq 1 $reps); do run1 "$label-v39" "$V39" "$W" "$envv" "$rep" "$arm"; done
}

cell_interleaved() {
    local label=$1 W=$2 envv=$3 reps=$4 arm=${5:-}
    echo "=== $label W=$W arm=${arm:-bigint} INTERLEAVED (loadavg $(awk '{print $1}' /proc/loadavg)) ===" >&2
    for rep in $(seq 1 $reps); do
        run1 "$label-base" "$BASE" "$W" "$envv" "$rep" "$arm"
        run1 "$label-v39" "$V39" "$W" "$envv" "$rep" "$arm"
    done
}

case "${1:-all}" in
    stab32)
        echo "=== W=32 stability v39 30 reps congc-DEFAULT (loadavg $(awk '{print $1}' /proc/loadavg)) ===" >&2
        for rep in $(seq 1 30); do run1 "stab32-v39" "$V39" 32 "$PINNED" "$rep"; done
        ;;
    bi-w1) cell giloff-bi-w1 1 "$PINNED" 3 ;;
    bi-w2) cell giloff-bi-w2 2 "$PINNED" 3 ;;
    bi-w4) cell giloff-bi-w4 4 "$PINNED" 3 ;;
    bi-w8) cell giloff-bi-w8 8 "$PINNED" 3 ;;
    bi-w16) cell giloff-bi-w16 16 "$PINNED" 3 ;;
    bi-w32) cell_interleaved giloff-bi-w32 32 "$PINNED" 3 ;;
    ic-w1) cell giloff-ic-w1 1 "$PINNED" 3 intcs ;;
    ic-w2) cell giloff-ic-w2 2 "$PINNED" 3 intcs ;;
    ic-w4) cell giloff-ic-w4 4 "$PINNED" 3 intcs ;;
    ic-w8) cell giloff-ic-w8 8 "$PINNED" 3 intcs ;;
    ic-w16) cell giloff-ic-w16 16 "$PINNED" 3 intcs ;;
    ic-w32) cell_interleaved giloff-ic-w32 32 "$PINNED" 3 intcs ;;
    gilon) cell_interleaved gilon-w1 1 "JSC_useJSThreads=1" 5 ;;
esac
