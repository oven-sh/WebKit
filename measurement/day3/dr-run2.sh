#!/bin/bash
# usage: dr-run2.sh <label> <timeout s> <workload.js> [jsc flags...]
# Instruction counts of the main thread with the JIT on: compilation and collection on the main thread, libpas
# scavenger off (patched copy of jsc). Buckets: inside JITWorklist::enqueue (compile), inside Heap::runCurrentPhase (gc).
label=$1; limit=$2; workload=$3; shift 3
cd /workspace/wkbuild/workloads
start=$(date +%s)
out=$(timeout $limit /workspace/wkbuild/dr/DynamoRIO-Linux-11.91.20715/bin64/drrun -c /workspace/wkbuild/dr/client2/build/libthreadcount.so compile=1beeaf0 gc=19ecfe0 -- /workspace/wkbuild/release-new/bin/jsc-noscav --useConcurrentJIT=0 --useConcurrentGC=0 --numberOfGCMarkers=1 "$@" $workload -- 1 2>&1 | grep "^threadcount\|^round" | tr '\n' ' ')
echo "$label | $(( $(date +%s) - start )) s | $out" >> /workspace/wkbuild/results/dr2.txt
