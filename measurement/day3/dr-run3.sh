#!/bin/bash
# usage: dr-run3.sh <label> <jsc binary> <workload.js> [jsc flags...]
# Instruction counts of the main thread, with buckets for the three LLInt slow paths and the collection.
# The binary needs the libpas scavenger off (a copy with the byte of pas_scavenger_is_enabled set to 0).
label=$1; jsc=$2; workload=$3; shift 3
cd /workspace/wkbuild/workloads
addr() { nm "$jsc" | grep -E " [Tt] $1\$" | head -1 | cut -d' ' -f1 | sed 's/^0*//'; }
gc=$(nm -C "$jsc" | grep -E " [Tt] JSC::Heap::runCurrentPhase\(" | head -1 | cut -d' ' -f1 | sed 's/^0*//')
get=$(addr llint_slow_path_get_by_id); put=$(addr llint_slow_path_put_by_id); len=$(addr llint_slow_path_get_length)
start=$(date +%s)
out=$(timeout 9000 /workspace/wkbuild/dr/DynamoRIO-Linux-11.91.20715/bin64/drrun -c /workspace/wkbuild/dr/client2/build/libthreadcount.so get_by_id=$get put_by_id=$put get_length=$len gc=$gc -- "$jsc" "$@" $workload -- 1 2>&1 | grep "^threadcount\|^round" | tr '\n' ' ')
echo "$label | $(( $(date +%s) - start )) s | $out" >> /workspace/wkbuild/results/dr3.txt
