#!/usr/bin/env bash
# §48 ship verification: intcs W=16 + flat W=16, 3 reps each, GIL-off pinned
# env, Release jsc. Checksums MUST match the §39b/§38 references; total_ms is
# recorded but NOT gated (the r48 fuzz campaign runs at nice 10 alongside).
set -u
SB="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JSC="$SB/../../../WebKitBuild/Release/bin/jsc"
PINNED="JSC_useJSThreads=1 JSC_useThreadGIL=0 JSC_useVMLite=1 JSC_useSharedAtomStringTable=1 JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1"
RAW="$SB/results-v48-ship.jsonl"
: > "$RAW"
ulimit -c 0

run1() {
    local label=$1 W=$2 arm=$3 rep=$4
    local out
    out=$(env $PINNED "$JSC" "$SB/js/bench.js" -- "$W" "$arm" 2>&1)
    local rc=$?
    local json=$(echo "$out" | grep '"impl"' | tail -1)
    [[ -z "$json" ]] && json=null
    echo "{\"label\":\"$label\",\"W\":$W,\"arm\":\"$arm\",\"rep\":$rep,\"rc\":$rc,\"load\":\"$(awk '{print $1}' /proc/loadavg)\",\"json\":$json}" | tee -a "$RAW"
}

echo "start loadavg: $(awk '{print $1}' /proc/loadavg)" >&2
for rep in 1 2 3; do run1 intcs-w16 16 intcs "$rep"; done
for rep in 1 2 3; do run1 flat-w16  16 flat  "$rep"; done
