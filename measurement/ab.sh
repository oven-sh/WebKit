#!/bin/bash
# Interleaved A/B runs with per-thread CPU and RSS.
# usage: ab.sh <pairs> <label> -- <command for A> -- <command for B>
# Prints one line per run, then medians.
pairs=$1; label=$2; shift 2
[ "$1" = "--" ] && shift
A=(); B=(); cur=A
for arg in "$@"; do
  if [ "$arg" = "--" ]; then cur=B; continue; fi
  if [ $cur = A ]; then A+=("$arg"); else B+=("$arg"); fi
done
out=/tmp/ab-$$.txt; : > $out
run() { # name cmd...
  name=$1; shift
  RUNSTAT_OUT=$out.one /workspace/wkbuild/tools/runstat "$@" > /dev/null 2>&1
  line=$(tail -1 $out.one); rm -f $out.one
  echo "$name $line" | tee -a $out
}
for i in $(seq 1 $pairs); do
  run "A" "${A[@]}"
  run "B" "${B[@]}"
done
# A/A pair
run "A2" "${A[@]}"
run "A2" "${A[@]}"
python3 - "$out" "$label" <<'PY'
import sys, re, statistics
rows = {}
for line in open(sys.argv[1]):
    name, rest = line.split(' ', 1)
    d = dict(kv.split('=') for kv in rest.split())
    rows.setdefault(name, []).append({k: float(v) for k, v in d.items()})
def med(name, key):
    return statistics.median(r[key] for r in rows[name])
def spread(name, key):
    v = [r[key] for r in rows[name]]
    return (max(v) - min(v)) / statistics.median(v) * 100 if statistics.median(v) else 0
print("== %s: medians over %d runs each (A/A pair: 2 runs)" % (sys.argv[2], len(rows['A'])))
for key in ('main_user', 'jit_cpu', 'gc_cpu', 'total_user', 'maxrss_kb', 'wall'):
    a, b = med('A', key), med('B', key)
    a2 = [r[key] for r in rows['A2']]
    print("  %-10s A=%10.2f  B=%10.2f  delta=%+6.1f%%   spread A=%4.1f%% B=%4.1f%%   A/A: %.2f vs %.2f (%+.1f%%)" % (
        key, a, b, (b - a) / a * 100 if a else 0, spread('A', key), spread('B', key), a2[0], a2[1], (a2[1] - a2[0]) / a2[0] * 100 if a2[0] else 0))
PY
rm -f $out
