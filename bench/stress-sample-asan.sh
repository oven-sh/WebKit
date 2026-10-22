#!/bin/bash
# usage: stress-sample-asan.sh <jsc> <outfile> <every nth> [extra options...]   every nth JSTests/stress test, 180 s timeout, 6 at a time
jsc=$1; out=$2; nth=$3; shift 3
export ASAN_OPTIONS=detect_leaks=0:allow_user_segv_handler=1:allocator_may_return_null=1
cd /workspace/WebKit/JSTests/stress
ls *.js | awk -v n=$nth 'NR % n == 0' | xargs -P 6 -I{} bash -c 's=$(date +%s%N); timeout 180 '"$jsc"' --useDollarVM=true '"$*"' {} > /tmp/asan-sample-{}.log 2>&1; c=$?; echo "$c $(( ($(date +%s%N) - s) / 1000000 )) {}"; if [ $c = 0 ] || [ $c = 3 ]; then rm -f /tmp/asan-sample-{}.log; fi' > $out
echo DONE >> $out
