#!/bin/bash
# One JSTests/stress test with collectContinuously and verifyGC at 8 markers on three Debug+ASAN arms at the same time: main, the PR, the PR with the option at 0.
t=$1; to=$2
export ASAN_OPTIONS=detect_leaks=0:allow_user_segv_handler=1:allocator_may_return_null=1
cd /workspace/WebKit/JSTests/stress
run() { local name=$1 jsc=$2; shift 2; ( TIMEFORMAT="$name $t: %R real %U user %S sys"; { time timeout $to $jsc --useDollarVM=true --numberOfGCMarkers=8 --collectContinuously=true --verifyGC=true "$@" $t > /tmp/trio-$name.log 2>&1; echo "$name $t: exit $?"; } 2>&1 | tail -2 ) & }
run main /workspace/dl/wk-main-asan/bun-webkit/bin/jsc
run pr /workspace/dl/wk-final-asan/bun-webkit/bin/jsc
run pr0 /workspace/dl/wk-final-asan/bun-webkit/bin/jsc --minimumBytesMarkedForParallelMarking=0
wait
