#!/bin/bash
J=/workspace/wkbuild/release-new/bin/jsc
OFF="--missCountForLLIntTierUp=0 --useLLIntUnsetCaching=0 --useLLIntStringLengthFastPath=0 --useLLIntPrototypeCacheRearming=0"
C=/workspace/wkbuild/tools/count2.sh
run() { "$@" > /dev/null 2>&1; }
WORKLOAD=tsc-small-census.js run $C "small: scale 10, threshold 50, four off" $J --thresholdForJITAfterWarmUp=50 --startupJITDeferralScale=10 $OFF
WORKLOAD=tsc-small-census.js run $C "small: scale 10, defaults, miss count 8" $J --thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10 --missCountForLLIntTierUp=8
WORKLOAD=tsc-small-census.js run $C "small: scale 10, defaults, miss count 24" $J --thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10 --missCountForLLIntTierUp=24
WORKLOAD=tsc-census.js run $C "full: scale 10, threshold 50, four off" $J --thresholdForJITAfterWarmUp=50 --startupJITDeferralScale=10 $OFF
echo "ref batch done $(date -u +%H:%M:%S)" >> /workspace/wkbuild/results/counts.txt
