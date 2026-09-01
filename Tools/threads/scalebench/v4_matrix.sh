#!/usr/bin/env bash
# Run 3.9 / scalebench matrix v4: full Go/Java/JS-BigInt/JS-intcs ladder, fresh
# numbers on jsc-v39 (sha 9152bed9, congc default-on). 3-rep medians, one
# process at a time, /usr/bin/time -v. Raw -> results-v4-raw.jsonl.
set -u
SB="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$SB/out"
JSC="$SB/../../../WebKitBuild/Release/bin/jsc-v39"
PINNED="JSC_useJSThreads=1 JSC_useThreadGIL=0 JSC_useVMLite=1 JSC_useSharedAtomStringTable=1 JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1"
RAW="$SB/results-v4-raw.jsonl"
ulimit -c 0

run1() {
    local lang=$1 W=$2 rep=$3 arm=$4 envv=$5; shift 5
    local tv out rc
    tv=$(mktemp)
    out=$(env $envv /usr/bin/time -v -o "$tv" "$@" 2>&1)
    rc=$?
    local rss=$(grep 'Maximum resident' "$tv" | awk '{print $NF}')
    local user=$(grep 'User time' "$tv" | awk '{print $NF}')
    local syss=$(grep 'System time' "$tv" | awk '{print $NF}')
    local elap=$(grep 'Elapsed (wall' "$tv" | sed 's/.*): //')
    local pct=$(grep 'Percent of CPU' "$tv" | awk '{print $NF}' | tr -d '%')
    rm -f "$tv"
    local json=$(echo "$out" | grep '"impl"' | tail -1)
    local total=$(echo "$json" | python3 -c 'import sys,json; print(json.load(sys.stdin)["total_ms"])' 2>/dev/null || echo null)
    local cs=$(echo "$json" | python3 -c 'import sys,json; j=json.load(sys.stdin); print("|".join(str(j[k]) for k in ["checksumA","postings","checksumA2","checksumB","checksumC"]))' 2>/dev/null || echo null)
    [[ -z "$json" ]] && json=null
    local errtail=""
    if [[ $rc -ne 0 ]]; then
        errtail=$(echo "$out" | grep -v '^JSC:' | tail -8 | tr '\n' '~' | sed 's/"/\\"/g')
    fi
    echo "{\"lang\":\"$lang\",\"W\":$W,\"arm\":\"$arm\",\"rep\":$rep,\"rc\":$rc,\"total_ms\":$total,\"rss_kb\":${rss:-null},\"user_s\":${user:-null},\"sys_s\":${syss:-null},\"elapsed\":\"$elap\",\"cpu_pct\":${pct:-null},\"cs\":\"$cs\",\"load\":\"$(awk '{print $1}' /proc/loadavg)\",\"err\":\"$errtail\",\"json\":$json}" | tee -a "$RAW"
}

cell() {
    local lang=$1 W=$2 reps=$3 arm=$4
    echo "=== $lang W=$W arm=$arm reps=$reps (loadavg $(awk '{print $1}' /proc/loadavg)) ===" >&2
    for rep in $(seq 1 $reps); do
        case "$lang" in
            go)       run1 go "$W" "$rep" "$arm" "" "$OUT/bench-go" "$W" ;;
            java)     run1 java "$W" "$rep" "$arm" "" java -cp "$OUT" Bench "$W" ;;
            js)       run1 js "$W" "$rep" "$arm" "$PINNED" "$JSC" "$SB/js/bench.js" -- "$W" $([ "$arm" = intcs ] && echo intcs) ;;
            js-gilon) run1 js-gilon "$W" "$rep" "$arm" "JSC_useJSThreads=1" "$JSC" "$SB/js/bench.js" -- "$W" ;;
        esac
    done
}

case "${1:-all}" in
    smoke)
        : > "$RAW.smoke"; RAW="$RAW.smoke"
        cell go 2 1 bigint
        cell java 2 1 bigint
        cell js 2 1 bigint
        cell js 2 1 intcs
        ;;
    go)    for W in 1 2 4 8 16 32; do cell go "$W" 3 bigint; done ;;
    java)  for W in 1 2 4 8 16 32; do cell java "$W" 3 bigint; done ;;
    jsbi)  for W in 1 2 4 8 16 32; do cell js "$W" 3 bigint; done ;;
    jsic)  for W in 1 2 4 8 16 32; do cell js "$W" 3 intcs; done ;;
    gilon)
        echo "=== GIL-on W=1 5 reps interleaved (loadavg $(awk '{print $1}' /proc/loadavg)) ===" >&2
        for rep in 1 2 3 4 5; do
            run1 js-gilon 1 "$rep" bigint "JSC_useJSThreads=1" "$JSC" "$SB/js/bench.js" -- 1
            run1 js 1 "g$rep" bigint "$PINNED" "$JSC" "$SB/js/bench.js" -- 1
        done
        ;;
esac
