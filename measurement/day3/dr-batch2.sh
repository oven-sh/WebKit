#!/bin/bash
R=/workspace/wkbuild/tools/dr-run2.sh
OFF="--missCountForLLIntTierUp=0 --useLLIntUnsetCaching=0 --useLLIntStringLengthFastPath=0 --useLLIntPrototypeCacheRearming=0"
ON="--missCountForLLIntTierUp=12 --useLLIntUnsetCaching=1 --useLLIntStringLengthFastPath=1 --useLLIntPrototypeCacheRearming=1"
lane1() {
  for w in tsc-small.js tsc-workload.js; do
    $R "$w: 5000, four off" 9000 $w --thresholdForJITAfterWarmUp=5000 $OFF
    $R "$w: 5000, defaults" 9000 $w --thresholdForJITAfterWarmUp=5000
    $R "$w: 500, four off" 9000 $w --thresholdForJITAfterWarmUp=500 $OFF
    $R "$w: 5000, four on" 9000 $w --thresholdForJITAfterWarmUp=5000 $ON
  done
}
lane2() {
  for w in tsc-small.js tsc-workload.js; do
    $R "$w: scale 10, four off" 9000 $w --thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10 $OFF
    $R "$w: scale 10, defaults" 9000 $w --thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10
    $R "$w: scale 10, defaults, deferral kept" 9000 $w --thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10 --useStartupJITDeferralAfterLLIntMisses=1
    $R "$w: 5000, four off (again)" 9000 $w --thresholdForJITAfterWarmUp=5000 $OFF
  done
}
lane1 &
lane2 &
wait
echo "dr2 batch done $(date -u +%H:%M:%S)" >> /workspace/wkbuild/results/dr2.txt
