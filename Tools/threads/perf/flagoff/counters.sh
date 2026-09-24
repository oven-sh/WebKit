#!/bin/bash
# counters.sh <label> <main|off> <test> [-- jsc options...]: one JetStream test, counters of the MAIN THREAD ONLY (perf stat -i).
# Prints: <test> <label> <score> <instructions:u> <cycles:u> <lock_loads:u> <task-clock ms> <wall s>
. "$(dirname "$0")/common.sh"
label=$1; cfg=$2; t=$3; shift 3; [ "$1" = -- ] && shift
J=$(bin_of "$cfg") || exit 1
cd "$JETSTREAM" || exit 1
tmp=$(mktemp)
out=$(perf stat -i -x, -o "$tmp" -e instructions:u,cycles:u,mem_inst_retired.lock_loads:u,task-clock "$J" "$@" -e "testList=[\"$t\"]" cli.js 2>&1)
s=$(echo "$out" | grep -m1 '^    Score:' | awk '{print $2}')
w=$(echo "$out" | grep -m1 'Wall time' | sed 's/.*Wall time: 0://')
i=$(grep instructions "$tmp" | cut -d, -f1); c=$(grep cycles "$tmp" | cut -d, -f1); l=$(grep lock_loads "$tmp" | cut -d, -f1); k=$(grep task-clock "$tmp" | cut -d, -f1); rm -f "$tmp"
echo "$t $label ${s:-FAIL} $i $c $l $k $w"
