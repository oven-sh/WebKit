#!/bin/bash
# Instruction counts with every tier on and compilation on the main thread (--useConcurrentJIT=0).
cd /workspace/wkbuild/workloads
run() { name="$1"; shift; start=$(date +%s); out=$( timeout 3000 /workspace/wkbuild/dr/DynamoRIO-Linux-11.91.20715/bin64/drrun -c /workspace/wkbuild/dr/client/build/libthreadcount.so -- /workspace/wkbuild/release-pin/bin/jsc --useConcurrentJIT=0 "$@" tsc-workload.js -- 1 2>&1 | grep "^threadcount\|^round" | tr '\n' ' ' ); echo "$name | $(( $(date +%s) - start )) s | $out" >> /workspace/wkbuild/dr-sync.log; }
run "5000, all off" --thresholdForJITAfterWarmUp=5000 --missCountForLLIntTierUp=0 --useLLIntUnsetCaching=0 --useLLIntStringLengthCaching=0 --useLLIntPrototypeCacheRearming=0
run "5000, all on" --thresholdForJITAfterWarmUp=5000
run "500, all off" --thresholdForJITAfterWarmUp=500 --missCountForLLIntTierUp=0 --useLLIntUnsetCaching=0 --useLLIntStringLengthCaching=0 --useLLIntPrototypeCacheRearming=0
run "5000, all off (A')" --thresholdForJITAfterWarmUp=5000 --missCountForLLIntTierUp=0 --useLLIntUnsetCaching=0 --useLLIntStringLengthCaching=0 --useLLIntPrototypeCacheRearming=0
run "5000, all on (B')" --thresholdForJITAfterWarmUp=5000
run "5000, tier-up only" --thresholdForJITAfterWarmUp=5000 --useLLIntUnsetCaching=0 --useLLIntStringLengthCaching=0 --useLLIntPrototypeCacheRearming=0
run "5000, 2+3+4 only" --thresholdForJITAfterWarmUp=5000 --missCountForLLIntTierUp=0
run "500, all on" --thresholdForJITAfterWarmUp=500
echo done >> /workspace/wkbuild/dr-sync.log
