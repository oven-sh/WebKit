#!/bin/bash
# phases.sh [rounds]: the 36 tests in ONE process (the gate's configuration), main-thread counters, main and flag off
# interleaved, for: the whole run, the first iteration only, the first six iterations, and each tier capped
# (LLInt only / up to Baseline / up to DFG), plus the peak resident set of the whole run.
# Output: $OUT/phases.txt lines "<phase> <cfg> <round> <instructions> <cycles> <lock_loads> <task-clock> <score>" and
# $OUT/rss.txt lines "<cfg> <peak-rss-kB>". Input of phases-compare.py.
. "$(dirname "$0")/common.sh"
R=${1:-3}; O=$OUT/phases.txt; : > "$O"; : > "$OUT/rss.txt"; LIST=$(testlist_js)
cd "$JETSTREAM" || exit 1
stat() { # phase cfg round js-prefix jsc-options...
  local phase=$1 cfg=$2 r=$3 js=$4; shift 4
  local J tmp=$(mktemp) out; J=$(bin_of "$cfg")
  out=$(perf stat -i -x, -o "$tmp" -e instructions:u,cycles:u,mem_inst_retired.lock_loads:u,task-clock "$J" "$@" -e "$js$LIST" cli.js 2>&1)
  local i c l k sc
  i=$(grep instructions "$tmp" | cut -d, -f1); c=$(grep cycles "$tmp" | cut -d, -f1); l=$(grep lock_loads "$tmp" | cut -d, -f1); k=$(grep task-clock "$tmp" | cut -d, -f1); rm -f "$tmp"
  sc=$(echo "$out" | grep -m1 "Total Score" | awk '{print $3}')
  echo "$phase $cfg $r $i $c $l $k ${sc:-NaN}" >> "$O"
}
for r in $(seq 1 "$R"); do
  for cfg in main off; do
    stat full "$cfg" "$r" ""
    stat first "$cfg" "$r" "testIterationCount=1;"
    stat six "$cfg" "$r" "testIterationCount=6;"
    for cap in useJIT useDFGJIT useFTLJIT; do stat "cap-$cap" "$cfg" "$r" "testIterationCount=6;" "--$cap=0"; done
  done
done
for cfg in main off; do
  python3 - "$(bin_of "$cfg")" "$LIST" "$cfg" >> "$OUT/rss.txt" <<'PY'
import resource, subprocess, sys
subprocess.run([sys.argv[1], "-e", sys.argv[2], "cli.js"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print(sys.argv[3], resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)
PY
done
echo "done: $O"
