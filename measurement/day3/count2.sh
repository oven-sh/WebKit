#!/bin/bash
# usage: count.sh <label> <jsc> [jsc flags...]
# Runs the tsc workload under gdb with --useConcurrentJIT=0, and prints the hit counts of the three slow paths,
# the number of Baseline compiles, and the JIT bytes of the census.
label=$1; jsc=$2; shift 2
out=/workspace/wkbuild/results/count-$$.txt
cd /workspace/wkbuild/workloads
gdb -q -batch -ex "set pagination off" -ex "set confirm off" \
  -ex "handle SIGUSR1 SIGUSR2 SIGPWR SIGXCPU SIGSEGV nostop noprint pass" \
  -ex "break llint_slow_path_get_by_id" -ex "break llint_slow_path_get_length" -ex "break llint_slow_path_put_by_id" \
  -ex "ignore 1 1000000000" -ex "ignore 2 1000000000" -ex "ignore 3 1000000000" \
  -ex "run" -ex "info breakpoints" \
  --args "$jsc" --useConcurrentJIT=0 --useDollarVM=1 --reportBaselineCompileTimes=1 "$@" ${WORKLOAD:-tsc-census.js} -- 1 > $out 2>&1
get=$(grep -A1 "llint_slow_path_get_by_id" $out | grep -oE "already hit [0-9]+" | head -1 | grep -oE "[0-9]+")
len=$(grep -A1 "llint_slow_path_get_length" $out | grep -oE "already hit [0-9]+" | head -1 | grep -oE "[0-9]+")
put=$(grep -A1 "llint_slow_path_put_by_id" $out | grep -oE "already hit [0-9]+" | head -1 | grep -oE "[0-9]+")
base=$(grep -c "using Baseline\|with Baseline\|Baseline into" $out)
census=$(grep "^census " $out | head -1)
jit=$(echo "$census" | grep -oE '"jitBytes":[0-9.e+]+' | cut -d: -f2)
tiers=$(echo "$census" | grep -oE '"(llint|baseline|dfg|ftl)":[0-9]+' | tr '\n' ' ')
round=$(grep "^round 0" $out | head -1)
echo "$label | get_by_id=$get get_length=$len put_by_id=$put baselineCompiles=$base jitBytes=$jit $tiers| $round"
echo "$label | get_by_id=$get get_length=$len put_by_id=$put baselineCompiles=$base jitBytes=$jit $tiers| $round" >> /workspace/wkbuild/results/counts.txt
mv $out /workspace/wkbuild/results/last-count-raw.txt
