#!/bin/bash
# Final rows for head 7da0e718d4, small workload.
R=/workspace/wkbuild/tools/dr-run4.sh
J=/workspace/wkbuild/release-new/bin/jsc-noscav
OFF="--missCountForLLIntTierUp=0 --useLLIntUnsetCaching=0 --useLLIntStringLengthFastPath=0 --useLLIntPrototypeCacheRearming=0"
(
$R "7da0e718: scale 10, four off" $J tsc-small.js --thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10 $OFF
$R "7da0e718: scale 10, defaults" $J tsc-small.js --thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10
$R "7da0e718: scale 10, defaults, no deferral after the misses" $J tsc-small.js --thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10 --useStartupJITDeferralAfterLLIntMisses=0
) &
(
$R "7da0e718: 500, four off" $J tsc-small.js --thresholdForJITAfterWarmUp=500 $OFF
$R "7da0e718: 500, defaults" $J tsc-small.js --thresholdForJITAfterWarmUp=500
) &
wait
echo "dr6 batch done $(date -u +%H:%M:%S)" >> /workspace/wkbuild/results/dr2.txt
