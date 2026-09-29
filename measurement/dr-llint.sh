#!/bin/bash
# LLInt-only instruction counts, after the CPU time runs are done (they must not share the machine).
while pgrep -f "tools/multi.py" > /dev/null; do sleep 10; done
cd /workspace/wkbuild/workloads
run() { name="$1"; shift; out=$( timeout 1500 /workspace/wkbuild/dr/DynamoRIO-Linux-11.91.20715/bin64/drrun -c /workspace/wkbuild/dr/client/build/libthreadcount.so -- /workspace/wkbuild/release-pin/bin/jsc --useJIT=0 "$@" tsc-workload.js -- 1 2>&1 | grep "^threadcount" ); echo "$name | $out" >> /workspace/wkbuild/dr-llint.log; }
for i in 1 2; do
run "all off" --missCountForLLIntTierUp=0 --useLLIntUnsetCaching=0 --useLLIntStringLengthCaching=0 --useLLIntPrototypeCacheRearming=0
run "all on"
run "unset only" --useLLIntStringLengthCaching=0 --useLLIntPrototypeCacheRearming=0
run "string length only" --useLLIntUnsetCaching=0 --useLLIntPrototypeCacheRearming=0
run "rearm only" --useLLIntUnsetCaching=0 --useLLIntStringLengthCaching=0
done
echo done >> /workspace/wkbuild/dr-llint.log
