#!/usr/bin/env bash
# §38 flat-gap-bughunter round 4 measurement driver.
# Round-4 worktree (per-realm cached Lock/Condition/Thread instance Structure
# + operationGetByIdPerThreadMegamorphic + phaseA-ingestHold-born-int32array
# + cs1-sum-ushr0-to-bitor0 + flatten2-serial→striped). 3-rep medians,
# GIL-off pinned env.
set -u
SB="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JSC="$SB/../../../WebKitBuild/Release/bin/jsc"
PINNED="JSC_useJSThreads=1 JSC_useThreadGIL=0 JSC_useVMLite=1 JSC_useSharedAtomStringTable=1 JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1"
RAW="$SB/results-v42ab-raw.jsonl"
: > "$RAW"
ulimit -c 0

run1() {
    local label=$1 W=$2 envv=$3 rep=$4 arm=${5:-}
    local out tv
    tv=$(mktemp)
    out=$(env $envv /usr/bin/time -v -o "$tv" "$JSC" "$SB/js/bench.js" -- "$W" $arm 2>&1)
    local rc=$?
    local rss=$(grep 'Maximum resident' "$tv" | awk '{print $NF}')
    local user=$(grep 'User time' "$tv" | awk '{print $NF}')
    local syss=$(grep 'System time' "$tv" | awk '{print $NF}')
    rm -f "$tv"
    local json=$(echo "$out" | grep '"impl"' | tail -1)
    [[ -z "$json" ]] && json=null
    local errtail=""
    if [[ $rc -ne 0 ]]; then
        errtail=$(echo "$out" | grep -v '^JSC:' | tail -8 | tr '\n' '~' | sed 's/"/\\"/g')
    fi
    echo "{\"label\":\"$label\",\"W\":$W,\"arm\":\"${arm:-default}\",\"rep\":$rep,\"rc\":$rc,\"rss_kb\":${rss:-null},\"user_s\":${user:-null},\"sys_s\":${syss:-null},\"load\":\"$(awk '{print $1}' /proc/loadavg)\",\"err\":\"$errtail\",\"json\":$json}" | tee -a "$RAW"
}

cell() {
    local label=$1 W=$2 reps=$3 arm=${4:-}
    echo "=== $label W=$W arm=${arm:-default} (loadavg $(awk '{print $1}' /proc/loadavg)) ===" >&2
    for rep in $(seq 1 $reps); do run1 "$label" "$W" "$PINNED" "$rep" "$arm"; done
}

# Wait for build-induced load to decay (synchronous; capped 180s).
for i in $(seq 1 36); do
    L=$(awk '{print $1}' /proc/loadavg)
    awk -v l="$L" 'BEGIN{exit !(l<2.5)}' && break
    sleep 5
done
echo "start loadavg: $(awk '{print $1}' /proc/loadavg)" >&2

case "${1:-all}" in
flat)
    for W in 1 8 16 32; do cell flat "$W" 3 flat; done
    ;;
stab32)
    cell flat-stab32 32 12 flat
    ;;
slowarms)
    for W in 1 16; do cell default "$W" 3; done
    for W in 1 16; do cell intcs "$W" 3 intcs; done
    ;;
all)
    for W in 1 8 16 32; do cell flat "$W" 3 flat; done
    cell flat-stab32 32 12 flat
    for W in 1 16; do cell default "$W" 3; done
    for W in 1 16; do cell intcs "$W" 3 intcs; done
    ;;
esac
