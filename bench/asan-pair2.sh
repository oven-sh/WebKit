#!/bin/bash
# One JSTests/stress test on the Debug+ASAN jsc of bun's pinned WebKit (main2) and of the PR head (new2), at the same time, with the same options.
# usage: asan-pair2.sh <test> <timeout s> [options...]
t=$1; to=$2; shift 2
export ASAN_OPTIONS=detect_leaks=0:allow_user_segv_handler=1:allocator_may_return_null=1
cd /workspace/WebKit/JSTests/stress
for arm in main2 new2; do ( TIMEFORMAT="$arm $t: %R real %U user %S sys"; { time timeout $to /workspace/dl/wk-$arm-asan/bun-webkit/bin/jsc --useDollarVM=true "$@" $t > /tmp/pair2-$arm-$t.log 2>&1; echo "$arm $t: exit $? $(grep -c "ERROR: AddressSanitizer\|ASSERTION FAILED" /tmp/pair2-$arm-$t.log) reports"; } 2>&1 | tail -2 ) & done
wait
