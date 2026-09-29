#!/bin/bash
# usage: dr-run4.sh <label> <jsc binary (scavenger off)> <workload.js> [jsc flags...]
# As dr-run2.sh, for any binary: buckets compile (JITWorklist::enqueue) and gc (Heap::runCurrentPhase).
label=$1; jsc=$2; workload=$3; shift 3
cd /workspace/wkbuild/workloads
syms=$(nm -C "$jsc")
compile=$(echo "$syms" | grep -E " [Tt] JSC::JITWorklist::enqueue\(" | head -1 | cut -d' ' -f1 | sed 's/^0*//')
gc=$(echo "$syms" | grep -E " [Tt] JSC::Heap::runCurrentPhase\(" | head -1 | cut -d' ' -f1 | sed 's/^0*//')
start=$(date +%s)
out=$(timeout 9000 /workspace/wkbuild/dr/DynamoRIO-Linux-11.91.20715/bin64/drrun -c /workspace/wkbuild/dr/client2/build/libthreadcount.so compile=$compile gc=$gc -- "$jsc" --useConcurrentJIT=0 --useConcurrentGC=0 --numberOfGCMarkers=1 "$@" $workload -- 1 2>&1 | grep "^threadcount\|^round" | tr '\n' ' ')
echo "$workload: $label | $(( $(date +%s) - start )) s | $out" >> /workspace/wkbuild/results/dr2.txt
