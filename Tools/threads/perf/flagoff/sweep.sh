#!/bin/bash
# sweep.sh [rounds] [parallelism]: main-thread counters for the 36 tests, main and flag off interleaved per round.
# Output: $OUT/counters.txt (input of counters-compare.py). Run on an otherwise idle machine.
. "$(dirname "$0")/common.sh"
R=${1:-2}; P=${2:-6}; O=$OUT/counters.txt; : > "$O"
for r in $(seq 1 "$R"); do
  for cfg in main off; do
    for t in $TESTS; do echo "$t"; done | xargs -P "$P" -I{} "$HERE/counters.sh" "$cfg" "$cfg" {} >> "$O"
  done
done
echo "done: $(wc -l < "$O") rows in $O"
