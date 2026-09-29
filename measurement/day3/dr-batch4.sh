#!/bin/bash
R=/workspace/wkbuild/tools/dr-run2.sh
OFF="--missCountForLLIntTierUp=0 --useLLIntUnsetCaching=0 --useLLIntStringLengthFastPath=0 --useLLIntPrototypeCacheRearming=0"
ON="--missCountForLLIntTierUp=12 --useLLIntUnsetCaching=1 --useLLIntStringLengthFastPath=1 --useLLIntPrototypeCacheRearming=1"
w=tsc-small.js
(
$R "$w: scale 10, miss count alone" 9000 $w --thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10 $OFF --missCountForLLIntTierUp=12
$R "$w: scale 10, four on" 9000 $w --thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10 $ON
) &
(
$R "$w: scale 10, string length alone" 9000 $w --thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10 $OFF --useLLIntStringLengthFastPath=1
$R "$w: scale 10, unset and rearming alone" 9000 $w --thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10 $OFF --useLLIntUnsetCaching=1 --useLLIntPrototypeCacheRearming=1
) &
wait
echo "dr4 batch done $(date -u +%H:%M:%S)" >> /workspace/wkbuild/results/dr2.txt
