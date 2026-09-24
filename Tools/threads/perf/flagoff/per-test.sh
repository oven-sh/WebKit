#!/bin/bash
# per-test.sh <label> <main|off> <test> [interp]: (a) first iteration only, main-thread counters; (b) with `interp`, the
# interpreter only (--useJIT=0, two iterations) instead; (c) otherwise a full single-test run's peak resident set.
# Prints: <test> <label> <instructions> <cycles> <lock_loads> [<peak rss kB>]
. "$(dirname "$0")/common.sh"
label=$1; cfg=$2; t=$3; mode=$4; J=$(bin_of "$cfg") || exit 1
cd "$JETSTREAM" || exit 1
tmp=$(mktemp)
if [ "$mode" = interp ]; then
  perf stat -i -x, -o "$tmp" -e instructions:u,cycles:u,mem_inst_retired.lock_loads:u "$J" --useJIT=0 -e "testIterationCount=2; testList=[\"$t\"]" cli.js > /dev/null 2>&1
else
  perf stat -i -x, -o "$tmp" -e instructions:u,cycles:u,mem_inst_retired.lock_loads:u "$J" -e "testIterationCount=1; testList=[\"$t\"]" cli.js > /dev/null 2>&1
fi
i=$(grep instructions "$tmp" | cut -d, -f1); c=$(grep cycles "$tmp" | cut -d, -f1); l=$(grep lock_loads "$tmp" | cut -d, -f1); rm -f "$tmp"
rss=""
if [ "$mode" != interp ]; then
  rss=$(python3 - "$J" "$t" <<'PY'
import resource, subprocess, sys
subprocess.run([sys.argv[1], "-e", 'testList=["%s"]' % sys.argv[2], "cli.js"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print(resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)
PY
)
fi
echo "$t $label $i $c $l $rss"
