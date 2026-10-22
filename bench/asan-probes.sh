#!/bin/bash
# An LRU program and a wide program on a Debug+ASAN jsc under 14 sets of GC options. usage: asan-probes.sh <jsc> <out>
jsc=$1; out=$2
export ASAN_OPTIONS=detect_leaks=0:allow_user_segv_handler=1:allocator_may_return_null=1
arms=(
"--numberOfGCMarkers=8"
"--numberOfGCMarkers=2"
"--numberOfGCMarkers=1"
"--numberOfGCMarkers=8 --minimumBytesMarkedForParallelMarking=0"
"--numberOfGCMarkers=8 --minimumBytesMarkedForParallelMarking=65536"
"--numberOfGCMarkers=8 --collectContinuously=true --verifyGC=true"
"--numberOfGCMarkers=8 --collectContinuously=true --verifyGC=true --minimumBytesMarkedForParallelMarking=0"
"--numberOfGCMarkers=8 --useConcurrentGC=false"
"--numberOfGCMarkers=8 --useStochasticMutatorScheduler=false"
"--numberOfGCMarkers=8 --gcIncrementScale=1"
"--numberOfGCMarkers=8 --minimumGCPauseMS=0"
"--numberOfGCMarkers=8 --gcMarkerPollMS=0"
"--numberOfGCMarkers=8 --slowPathAllocsBetweenGCs=200"
"--numberOfGCMarkers=8 --useGenerationalGC=false"
)
: > $out
for a in "${arms[@]}"; do for p in lru.jsc.js wide.jsc.js; do
  ( s=$(date +%s); timeout 900 $jsc --useDollarVM=true $a /tmp/repro/$p > /tmp/probe.$$.log 2>&1; c=$?; echo "$c $(( $(date +%s) - s ))s $p [$a] $(tail -1 /tmp/probe.$$.log | cut -c1-80)" >> $out; if [ $c != 0 ]; then cp /tmp/probe.$$.log "/tmp/probe-fail-$p-$(echo $a | tr -c 'A-Za-z0-9' '_').log"; fi )
done; done
echo DONE >> $out
