#!/bin/bash
# memory.sh [rounds]: peak resident set (ru_maxrss) of the whole 36-test run and of eight single tests, main and flag off interleaved,
# medians of the rounds. Output: $OUT/memory.txt "<test|ALL> <cfg> <round> <peak kB>" and a table on stdout.
. "$(dirname "$0")/common.sh"
R=${1:-5}; O=$OUT/memory.txt; : > "$O"; LIST=$(testlist_js)
cd "$JETSTREAM" || exit 1
peak() { python3 - "$1" "$2" <<'PY'
import resource, subprocess, sys
subprocess.run([sys.argv[1], "-e", sys.argv[2], "cli.js"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print(resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)
PY
}
for r in $(seq 1 "$R"); do
  for cfg in main off; do
    J=$(bin_of "$cfg")
    echo "ALL $cfg $r $(peak "$J" "$LIST")" >> "$O"
    for t in splay Air Babylon typescript earley-boyer pdfjs raytrace gbemu; do
      echo "$t $cfg $r $(peak "$J" "testList=[\"$t\"]")" >> "$O"
    done
  done
done
python3 - "$O" <<'PY'
import sys, collections, statistics
d = collections.defaultdict(list)
for l in open(sys.argv[1]):
    t, c, r, kb = l.split(); d[(t, c)].append(int(kb))
print("%-14s %10s %10s %8s   (median peak RSS in MB, %d rounds)" % ("test", "main", "flag off", "off/main", len(next(iter(d.values())))))
for t in sorted({k[0] for k in d}, key=lambda t: (t != "ALL", t)):
    m = statistics.median(d[(t, "main")]) / 1024; o = statistics.median(d[(t, "off")]) / 1024
    print("%-14s %10.0f %10.0f %8.3f" % (t, m, o, o / m))
PY
