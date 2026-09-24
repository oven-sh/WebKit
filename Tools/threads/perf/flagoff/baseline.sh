#!/bin/bash
# baseline.sh [tag]: the whole flag-off measurement set on an idle machine, in the order of FLAG-OFF-LANDING 2.2:
# start-up, main-thread counters per test, the phases of one process, then the quiet score pass (five rounds).
# Run it with nothing else on the machine; it refuses to start under load. Results go to $OUT (default ./flagoff-out).
. "$(dirname "$0")/common.sh"
load=$(cut -d' ' -f1 /proc/loadavg)
if awk "BEGIN{exit !($load > 2.0)}"; then echo "machine not idle (load $load)"; [ -n "$FORCE" ] || exit 1; fi
echo "$(date +%H:%M) start $1" >> "$OUT/baseline.status"
"$HERE/startup.sh" 15 > "$OUT/startup.summary" 2>&1;            echo "$(date +%H:%M) startup done" >> "$OUT/baseline.status"
"$HERE/sweep.sh" 2 6 > "$OUT/sweep.log" 2>&1;                    echo "$(date +%H:%M) counters done" >> "$OUT/baseline.status"
"$HERE/phases.sh" 3 > "$OUT/phases.log" 2>&1;                    echo "$(date +%H:%M) phases done" >> "$OUT/baseline.status"
"$HERE/quiet-pass.sh" 5;                                         echo "$(date +%H:%M) quiet pass done" >> "$OUT/baseline.status"
python3 "$HERE/counters-compare.py" > "$OUT/counters.summary"
python3 "$HERE/phases-compare.py" > "$OUT/phases.summary"
python3 "$HERE/score-compare.py" > "$OUT/score.summary"
echo "$(date +%H:%M) ALLDONE" >> "$OUT/baseline.status"
