#!/bin/bash
# usage: dr-run.sh <label> <timeout s> [jsc flags...]   (tsc workload, instruction counts for each thread)
label=$1; limit=$2; shift 2
cd /workspace/wkbuild/workloads
start=$(date +%s)
out=$(timeout $limit /workspace/wkbuild/dr/DynamoRIO-Linux-11.91.20715/bin64/drrun -c /workspace/wkbuild/dr/client/build/libthreadcount.so -- ${JSCBIN:-/workspace/wkbuild/release-new/bin/jsc} "$@" tsc-workload.js -- 1 2>&1 | grep "^threadcount\|^round" | tr '\n' ' ')
echo "$label | exit ${PIPESTATUS[0]} | $(( $(date +%s) - start )) s | $out" >> /workspace/wkbuild/results/dr.txt
