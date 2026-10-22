#!/bin/bash
# Runs one JSTests/stress test on the Debug+ASAN jsc of main and of the PR with the same options, at the same time.
# usage: asan-pair.sh <test> <timeout s> <options for both> -- <options for the PR only>
t=$1; to=$2; shift 2; both=(); pr=(); seen=0
for a in "$@"; do if [ "$a" = "--" ]; then seen=1; elif [ $seen = 0 ]; then both+=("$a"); else pr+=("$a"); fi; done
export ASAN_OPTIONS=detect_leaks=0:allow_user_segv_handler=1:allocator_may_return_null=1
cd /workspace/WebKit/JSTests/stress
( TIMEFORMAT="main $t: %R real %U user %S sys"; { time timeout $to /workspace/dl/wk-main-asan/bun-webkit/bin/jsc --useDollarVM=true "${both[@]}" $t > /tmp/pair-main-$t.log 2>&1; echo "main $t: exit $?"; } 2>&1 | tail -2 ) &
( TIMEFORMAT="pr   $t: %R real %U user %S sys"; { time timeout $to /workspace/dl/wk-final-asan/bun-webkit/bin/jsc --useDollarVM=true "${both[@]}" "${pr[@]}" $t > /tmp/pair-pr-$t.log 2>&1; echo "pr   $t: exit $?"; } 2>&1 | tail -2 ) &
wait
