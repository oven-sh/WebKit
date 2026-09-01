#!/usr/bin/env bash
# (C) control: 15 reps W=16 intcs WITHOUT JSC_logGC, to baseline fast-mode rate.
set -u
SB="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JSC="$SB/../../../WebKitBuild/Release/bin/jsc-v39"
PINNED="JSC_useJSThreads=1 JSC_useThreadGIL=0 JSC_useVMLite=1 JSC_useSharedAtomStringTable=1 JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1"
RAW="$SB/results-v4-raw.jsonl"
EXTRA="${EXTRA:-}"
ARM="${ARM:-intcs-ctrl}"
ulimit -c 0

for rep in $(seq 1 ${1:-15}); do
    tv=$(mktemp)
    out=$(env $PINNED $EXTRA /usr/bin/time -v -o "$tv" "$JSC" "$SB/js/bench.js" -- 16 intcs 2>&1)
    rc=$?
    rss=$(grep 'Maximum resident' "$tv" | awk '{print $NF}')
    user=$(grep 'User time' "$tv" | awk '{print $NF}')
    syss=$(grep 'System time' "$tv" | awk '{print $NF}')
    pct=$(grep 'Percent of CPU' "$tv" | awk '{print $NF}' | tr -d '%')
    json=$(echo "$out" | grep '"impl"' | tail -1)
    total=$(echo "$json" | python3 -c 'import sys,json; print(json.load(sys.stdin)["total_ms"])' 2>/dev/null || echo null)
    pA=$(echo "$json" | python3 -c 'import sys,json; print(json.load(sys.stdin)["phaseA_ms"])' 2>/dev/null || echo null)
    [[ -z "$json" ]] && json=null
    rm -f "$tv"
    echo "{\"lang\":\"js\",\"W\":16,\"arm\":\"$ARM\",\"rep\":$rep,\"rc\":$rc,\"total_ms\":$total,\"phaseA_ms\":$pA,\"rss_kb\":${rss:-null},\"user_s\":${user:-null},\"sys_s\":${syss:-null},\"cpu_pct\":${pct:-null},\"load\":\"$(awk '{print $1}' /proc/loadavg)\",\"json\":$json}" | tee -a "$RAW"
done
