#!/bin/bash
J=/workspace/wkbuild/release-new/bin/jsc
OFF="--missCountForLLIntTierUp=0 --useLLIntUnsetCaching=0 --useLLIntStringLengthFastPath=0 --useLLIntPrototypeCacheRearming=0"
C=/workspace/wkbuild/tools/count2.sh
run() { "$@" > /dev/null 2>&1; }
for w in small full; do
  if [ $w = small ]; then export WORKLOAD=tsc-small-census.js; else export WORKLOAD=tsc-census.js; fi
  run $C "7da0e718 $w: scale 10, four off" $J --thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10 $OFF
  run $C "7da0e718 $w: scale 10, defaults" $J --thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10
  run $C "7da0e718 $w: scale 10, defaults, no deferral after the misses" $J --thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10 --useStartupJITDeferralAfterLLIntMisses=0
  run $C "7da0e718 $w: 500, four off" $J --thresholdForJITAfterWarmUp=500 $OFF
  run $C "7da0e718 $w: 500, defaults" $J --thresholdForJITAfterWarmUp=500
done
echo "final batch done $(date -u +%H:%M:%S)" >> /workspace/wkbuild/results/counts.txt
