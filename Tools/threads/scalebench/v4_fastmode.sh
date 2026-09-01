#!/usr/bin/env bash
# (C) W=16 fast-mode characterization: 15 consecutive reps JS intcs W=16,
# JSC_logGC=1 piped to GC count. Record total_ms, phaseA_ms, sys_s, GC counts,
# cpu_pct. Classify fast (phaseA<4800) vs normal.
set -u
SB="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JSC="$SB/../../../WebKitBuild/Release/bin/jsc-v39"
PINNED="JSC_useJSThreads=1 JSC_useThreadGIL=0 JSC_useVMLite=1 JSC_useSharedAtomStringTable=1 JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1"
RAW="$SB/results-v4-raw.jsonl"
ulimit -c 0

for rep in $(seq 1 ${1:-15}); do
    tv=$(mktemp); gclog=$(mktemp)
    out=$(env $PINNED JSC_logGC=1 /usr/bin/time -v -o "$tv" "$JSC" "$SB/js/bench.js" -- 16 intcs 2>"$gclog")
    rc=$?
    rss=$(grep 'Maximum resident' "$tv" | awk '{print $NF}')
    user=$(grep 'User time' "$tv" | awk '{print $NF}')
    syss=$(grep 'System time' "$tv" | awk '{print $NF}')
    pct=$(grep 'Percent of CPU' "$tv" | awk '{print $NF}' | tr -d '%')
    eden=$(grep -c 'EdenCollection' "$gclog" || true)
    full=$(grep -c 'FullCollection' "$gclog" || true)
    json=$(echo "$out" | grep '"impl"' | tail -1)
    total=$(echo "$json" | python3 -c 'import sys,json; print(json.load(sys.stdin)["total_ms"])' 2>/dev/null || echo null)
    pA=$(echo "$json" | python3 -c 'import sys,json; print(json.load(sys.stdin)["phaseA_ms"])' 2>/dev/null || echo null)
    cs=$(echo "$json" | python3 -c 'import sys,json; j=json.load(sys.stdin); print("|".join(str(j[k]) for k in ["checksumA","postings","checksumA2","checksumB","checksumC"]))' 2>/dev/null || echo null)
    [[ -z "$json" ]] && json=null
    rm -f "$tv" "$gclog"
    echo "{\"lang\":\"js\",\"W\":16,\"arm\":\"intcs-fm\",\"rep\":$rep,\"rc\":$rc,\"total_ms\":$total,\"phaseA_ms\":$pA,\"rss_kb\":${rss:-null},\"user_s\":${user:-null},\"sys_s\":${syss:-null},\"cpu_pct\":${pct:-null},\"gc_eden\":$eden,\"gc_full\":$full,\"cs\":\"$cs\",\"load\":\"$(awk '{print $1}' /proc/loadavg)\",\"json\":$json}" | tee -a "$RAW"
done
