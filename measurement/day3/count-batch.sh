#!/bin/bash
J=/workspace/wkbuild/release-new/bin/jsc
OFF="--missCountForLLIntTierUp=0 --useLLIntUnsetCaching=0 --useLLIntStringLengthFastPath=0 --useLLIntPrototypeCacheRearming=0"
ON="--missCountForLLIntTierUp=12 --useLLIntUnsetCaching=1 --useLLIntStringLengthFastPath=1 --useLLIntPrototypeCacheRearming=1"
C=/workspace/wkbuild/tools/count.sh
run() { "$@" > /dev/null 2>&1; }
(
run $C "5000, four off (again)" $J --thresholdForJITAfterWarmUp=5000 $OFF
run $C "5000, defaults" $J --thresholdForJITAfterWarmUp=5000
run $C "5000, four on" $J --thresholdForJITAfterWarmUp=5000 $ON
run $C "500, four off" $J --thresholdForJITAfterWarmUp=500 $OFF
run $C "500, defaults" $J --thresholdForJITAfterWarmUp=500
run $C "scale 10, four off" $J --thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10 $OFF
run $C "scale 10, defaults" $J --thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10
run $C "scale 10, defaults, deferral after misses" $J --thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10 --useStartupJITDeferralAfterLLIntMisses=1
) &
(
run $C "scale 10, four on" $J --thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10 $ON
run $C "5000, miss count only" $J --thresholdForJITAfterWarmUp=5000 $OFF --missCountForLLIntTierUp=12
run $C "5000, string length only" $J --thresholdForJITAfterWarmUp=5000 $OFF --useLLIntStringLengthFastPath=1
run $C "5000, unset only" $J --thresholdForJITAfterWarmUp=5000 $OFF --useLLIntUnsetCaching=1
run $C "5000, rearming only" $J --thresholdForJITAfterWarmUp=5000 $OFF --useLLIntPrototypeCacheRearming=1
run $C "5000, defaults, miss count 8" $J --thresholdForJITAfterWarmUp=5000 --missCountForLLIntTierUp=8
run $C "5000, defaults, miss count 16" $J --thresholdForJITAfterWarmUp=5000 --missCountForLLIntTierUp=16
run $C "5000, defaults, miss count 32" $J --thresholdForJITAfterWarmUp=5000 --missCountForLLIntTierUp=32
) &
wait
echo "batch done $(date +%H:%M:%S)" >> /workspace/wkbuild/results/counts.txt
