#!/usr/bin/env python3
"""counters-compare.py [-v] [file]: geometric means over tests of main-thread instructions, cycles, task-clock, score and
locked loads, flag off relative to main (minima over runs; score: maxima). -v adds the per-test rows."""
import collections, math, os, sys
args = [a for a in sys.argv[1:] if not a.startswith("-")]
path = args[0] if args else os.path.join(os.environ.get("OUT", "flagoff-out"), "counters.txt")
d = collections.defaultdict(lambda: collections.defaultdict(list))
for l in open(path):
    p = l.split()
    if len(p) < 8 or not p[3].isdigit(): continue
    try: d[p[1]][p[0]].append((float(p[2]), float(p[3]), float(p[4]), float(p[5]), float(p[6])))
    except ValueError: continue
def g(x): return math.exp(sum(math.log(v) for v in x) / len(x))
tests = sorted(d["main"])
def mn(c, t, k): return min(r[k] for r in d[c][t])
def mx(c, t, k): return max(r[k] for r in d[c][t])
print("%-8s %7s %7s %8s %7s %9s" % ("config", "instr", "cycles", "taskclk", "score", "lock/kins"))
for c in [c for c in ("main", "off") if c in d]:
    ts = [t for t in tests if t in d[c]]
    ri = g([mn(c, t, 1) / mn("main", t, 1) for t in ts]); rc = g([mn(c, t, 2) / mn("main", t, 2) for t in ts])
    rk = g([mn(c, t, 4) / mn("main", t, 4) for t in ts]); rs = g([mx(c, t, 0) / mx("main", t, 0) for t in ts])
    lk = g([1000 * max(mn(c, t, 3), 1) / mn(c, t, 1) for t in ts])
    print("%-8s %7.4f %7.4f %8.4f %7.4f %9.3f  n=%d" % (c, ri, rc, rk, rs, lk, len(ts)))
if "-v" in sys.argv and "off" in d:
    print("%-28s %7s %7s %7s" % ("test (off/main)", "instr", "cycles", "score"))
    for t in tests:
        if t in d["off"]:
            print("%-28s %7.3f %7.3f %7.3f" % (t, mn("off", t, 1) / mn("main", t, 1), mn("off", t, 2) / mn("main", t, 2), mx("off", t, 0) / mx("main", t, 0)))
