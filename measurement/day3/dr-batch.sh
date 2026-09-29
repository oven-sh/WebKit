#!/bin/bash
# Instruction counts with the JIT on: compilation and collection on the main thread, libpas scavenger off (patched copy of jsc).
export JSCBIN=/workspace/wkbuild/release-new/bin/jsc-noscav
R=/workspace/wkbuild/tools/dr-run.sh
BASE="--useConcurrentJIT=0 --useConcurrentGC=0 --numberOfGCMarkers=1"
OFF="--missCountForLLIntTierUp=0 --useLLIntUnsetCaching=0 --useLLIntStringLengthFastPath=0 --useLLIntPrototypeCacheRearming=0"
ON="--missCountForLLIntTierUp=12 --useLLIntUnsetCaching=1 --useLLIntStringLengthFastPath=1 --useLLIntPrototypeCacheRearming=1"
(
$R "noscav, 5000, four off" 6000 $BASE --thresholdForJITAfterWarmUp=5000 $OFF
$R "noscav, scale 10, defaults" 6000 $BASE --thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10
) &
(
$R "noscav, scale 10, four off" 6000 $BASE --thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10 $OFF
$R "noscav, scale 10, defaults, deferral kept" 6000 $BASE --thresholdForJITAfterWarmUp=500 --startupJITDeferralScale=10 --useStartupJITDeferralAfterLLIntMisses=1
) &
(
$R "noscav, 500, four off" 6000 $BASE --thresholdForJITAfterWarmUp=500 $OFF
$R "noscav, 5000, four on" 6000 $BASE --thresholdForJITAfterWarmUp=5000 $ON
) &
wait
echo "dr batch done $(date -u +%H:%M:%S)" >> /workspace/wkbuild/results/dr.txt
