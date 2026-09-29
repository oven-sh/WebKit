#!/bin/bash
R=/workspace/wkbuild/tools/dr-run2.sh
OFF="--missCountForLLIntTierUp=0 --useLLIntUnsetCaching=0 --useLLIntStringLengthFastPath=0 --useLLIntPrototypeCacheRearming=0"
w=tsc-small.js
$R "$w: scale 10, threshold 50, four off" 9000 $w --thresholdForJITAfterWarmUp=50 --startupJITDeferralScale=10 $OFF
$R "$w: scale 10, defaults, miss count 8" 9000 $w --thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10 --missCountForLLIntTierUp=8
$R "$w: scale 10, defaults, miss count 24" 9000 $w --thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10 --missCountForLLIntTierUp=24
echo "dr3 batch done $(date -u +%H:%M:%S)" >> /workspace/wkbuild/results/dr2.txt
