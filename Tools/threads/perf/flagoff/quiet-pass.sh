#!/bin/bash
# quiet-pass.sh [rounds]: full JetStream (the 36 tests, one process), main and flag off interleaved, on a quiet machine.
# Output: $OUT/quiet/js-<round>-<main|off>.txt (input of score-compare.py). Medians of five are the gate's measure.
. "$(dirname "$0")/common.sh"
N=${1:-5}; O=$OUT/quiet; mkdir -p "$O"; LIST=$(testlist_js)
cd "$JETSTREAM" || exit 1
echo "$(date +%H:%M) start (load $(cut -d' ' -f1 /proc/loadavg))" >> "$O/status"
for r in $(seq 1 "$N"); do
  "$JSC_MAIN" -e "$LIST" cli.js > "$O/js-$r-main.txt" 2>&1
  "$JSC_BRANCH" -e "$LIST" cli.js > "$O/js-$r-off.txt" 2>&1
  echo "$(date +%H:%M) round $r done (load $(cut -d' ' -f1 /proc/loadavg))" >> "$O/status"
done
echo ALLDONE >> "$O/status"
