#!/bin/bash
# run-micro.sh <outdir> [runs] : five configurations (main, main with polling traps, branch flag-off, GIL-on, GIL-off) x (bench-gate set + micro-extra), RUNS runs each, "cfg bench ms" lines.
set -u
OUT=$1; RUNS=${2:-5}; mkdir -p "$OUT"
R=$(cd "$(dirname "$0")/../../.." && pwd)
MAIN=${MAINJSC:?set MAINJSC to the main-tree jsc}   # jsc built from the merge base
BR=${BRJSC:?set BRJSC to the branch jsc}
GILOFF="JSC_useSharedGCHeap=1 JSC_useThreadGILOffUnsafe=1 JSC_useThreadGIL=0"
benches=$(ls $R/JSTests/threads/bench/*.js | grep -v harness.js)
run() { # cfg jsc envs opts
  local cfg=$1 jsc=$2 envs=$3 opts=$4
  for i in $(seq 1 $RUNS); do
    for b in $benches; do
      env $envs $jsc $opts $R/JSTests/threads/bench/harness.js $b 2>/dev/null | grep '^BENCH' | sed "s/^BENCH/$cfg/"
    done
    env $envs $jsc $opts $R/Tools/threads/perf/micro-extra.js 2>/dev/null | grep '^BENCH' | sed "s/^BENCH/$cfg/"
  done
}
{
run main   $MAIN "" ""
run mainpoll $MAIN "" "--usePollingTraps=1"
run off    $BR   "" ""
run gilon  $BR   "" "--useJSThreads=1"
run giloff $BR   "$GILOFF" "--useJSThreads=1"
} | tee "$OUT/raw.txt"
python3 - "$OUT/raw.txt" <<'PY'
import sys, statistics, collections
d = collections.defaultdict(list)
for line in open(sys.argv[1]):
    p = line.split()
    if len(p) == 3: d[(p[0], p[1])].append(float(p[2]))
benches = sorted({b for (_, b) in d})
cfgs = ["main", "mainpoll", "off", "gilon", "giloff"]
print("| benchmark | main | main, polling traps | flag off | GIL on | GIL off | off/main | on/mainpoll | GIL off/on |")
print("|---|---|---|---|---|---|---|---|---|")
for b in benches:
    m = {c: (statistics.median(d[(c,b)]) if d[(c,b)] else float('nan')) for c in cfgs}
    r = lambda a, b_: (m[a]/m[b_]) if m[b_] == m[b_] and m[b_] else float('nan')
    print(f"| {b} | {m['main']:.1f} | {m['mainpoll']:.1f} | {m['off']:.1f} | {m['gilon']:.1f} | {m['giloff']:.1f} | {r('off','main'):.2f} | {r('gilon','mainpoll'):.2f} | {r('giloff','gilon'):.2f} |")
PY
