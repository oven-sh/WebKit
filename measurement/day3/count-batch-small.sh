#!/bin/bash
export WORKLOAD=tsc-small-census.js
J=/workspace/wkbuild/release-new/bin/jsc
OFF="--missCountForLLIntTierUp=0 --useLLIntUnsetCaching=0 --useLLIntStringLengthFastPath=0 --useLLIntPrototypeCacheRearming=0"
ON="--missCountForLLIntTierUp=12 --useLLIntUnsetCaching=1 --useLLIntStringLengthFastPath=1 --useLLIntPrototypeCacheRearming=1"
C=/workspace/wkbuild/tools/count2.sh
run() { "$@" > /dev/null 2>&1; }
run $C "small: 5000, four off" $J --thresholdForJITAfterWarmUp=5000 $OFF
run $C "small: 5000, defaults" $J --thresholdForJITAfterWarmUp=5000
run $C "small: 5000, four on" $J --thresholdForJITAfterWarmUp=5000 $ON
run $C "small: 500, four off" $J --thresholdForJITAfterWarmUp=500 $OFF
run $C "small: scale 10, four off" $J --thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10 $OFF
run $C "small: scale 10, defaults" $J --thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10
run $C "small: scale 10, defaults, deferral kept" $J --thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10 --useStartupJITDeferralAfterLLIntMisses=1
echo "small batch done $(date -u +%H:%M:%S)" >> /workspace/wkbuild/results/counts.txt
