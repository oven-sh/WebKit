#!/bin/bash
# startup.sh [runs]: shell start-up cost, main against flag off, interleaved: user-mode instructions (minimum and median over
# `runs`, default 15) and peak resident set (median of 5) for an empty script and for a thousand-iteration loop that prints,
# in the caller's environment without any JSC_ variable, and for the empty script once more with one JSC_ option set
# ("envopt": the shell then looks every option up in the environment, which the branch otherwise skips).
# Output: $OUT/startup.txt, one line per (script, configuration): "<script> <cfg> <instr min> <instr median> <rss kB median>".
. "$(dirname "$0")/common.sh"
N=${1:-15}; O=$OUT/startup.txt; : > "$O"
declare -A SCRIPT=([empty]="" [loop]='var s=0;for(var i=0;i<1000;i++)s+=i;print(s)' [envopt]="")
for v in $(env | sed -n 's/^\(JSC_[A-Za-z0-9_]*\)=.*/\1/p'); do unset "$v"; done
for name in empty loop envopt; do
  if [ $name = envopt ]; then export JSC_dumpOptions=0; else unset JSC_dumpOptions; fi
  for cfg in main off; do
    J=$(bin_of "$cfg"); js=${SCRIPT[$name]}
    ins=(); rss=()
    for i in $(seq 1 "$N"); do
      tmp=$(mktemp)
      perf stat -x, -o "$tmp" -e instructions:u "$J" -e "$js" > /dev/null 2>&1
      ins+=($(grep instructions "$tmp" | cut -d, -f1)); rm -f "$tmp"
    done
    for i in 1 2 3 4 5; do
      rss+=($(python3 - "$J" "$js" <<'PY'
import resource, subprocess, sys
subprocess.run([sys.argv[1], "-e", sys.argv[2]], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print(resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)
PY
))
    done
    python3 - "$name" "$cfg" "${ins[*]}" "${rss[*]}" >> "$O" <<'PY'
import statistics, sys
i = [int(x) for x in sys.argv[3].split()]; r = [int(x) for x in sys.argv[4].split()]
print(sys.argv[1], sys.argv[2], min(i), int(statistics.median(i)), int(statistics.median(r)))
PY
  done
done
python3 - "$O" <<'PY'
import sys
d = {}
for l in open(sys.argv[1]):
    s, c, mi, md, r = l.split(); d[(s, c)] = (int(mi), int(md), int(r))
for s in ("empty", "loop", "envopt"):
    m, o = d[(s, "main")], d[(s, "off")]
    print("%-6s instructions main %.2f M  flag off %.2f M  (%+.2f %%)   rss main %.1f MB  flag off %.1f MB  (%+.1f MB)" % (s, m[0] / 1e6, o[0] / 1e6, 100 * (o[0] / m[0] - 1), m[2] / 1024, o[2] / 1024, (o[2] - m[2]) / 1024))
PY
