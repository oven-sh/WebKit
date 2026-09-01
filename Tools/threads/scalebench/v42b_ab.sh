#!/usr/bin/env bash
# §42 giloff-tax-bughunter measurement driver.
# H-VMLITE-TLCPTR candidate (lite-relative TLC LocalAllocator JIT bake,
# GILOFF-TAX #1/#2). 3-rep medians, /usr/bin/time -v, GIL-off pinned env
# + GIL-on baseline.
set -u
SB="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JSC="$SB/../../../WebKitBuild/Release/bin/jsc"
PINNED="JSC_useJSThreads=1 JSC_useThreadGIL=0 JSC_useVMLite=1 JSC_useSharedAtomStringTable=1 JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1"
GILON="JSC_useJSThreads=1"
RAW="$SB/results-v42b-raw.jsonl"
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
    local label=$1 W=$2 envv=$3 reps=$4 arm=${5:-}
    echo "=== $label W=$W arm=${arm:-default} (loadavg $(awk '{print $1}' /proc/loadavg)) ===" >&2
    for rep in $(seq 1 $reps); do run1 "$label" "$W" "$envv" "$rep" "$arm"; done
}

# Wait for build-induced load to decay (synchronous; capped 300s).
for i in $(seq 1 60); do
    L=$(awk '{print $1}' /proc/loadavg)
    awk -v l="$L" 'BEGIN{exit !(l<2.5)}' && break
    sleep 5
done
echo "start loadavg: $(awk '{print $1}' /proc/loadavg)" >&2

# §42 matrix
cell intcs-off    1 "$PINNED" 3 intcs
cell intcs-off   16 "$PINNED" 3 intcs
cell nomap-off    1 "$PINNED" 3 nomap
cell nomap-off   16 "$PINNED" 3 nomap
cell intcs-gilon  1 "$GILON"  3 intcs
cell flat-off    16 "$PINNED" 3 flat
cell default-off  1 "$PINNED" 3
