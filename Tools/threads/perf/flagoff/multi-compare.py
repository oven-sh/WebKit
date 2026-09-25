#!/usr/bin/env python3
"""multi-compare.py <dir>: each binary / main per phase (minima over rounds): instructions, cycles, locked loads; median score for full."""
import collections, os, statistics, sys
d = collections.defaultdict(lambda: collections.OrderedDict())
for l in open(os.path.join(sys.argv[1], "multi.txt")):
    p = l.split()
    if len(p) < 8 or not p[3].isdigit(): continue
    d[p[0]].setdefault(p[1], []).append((float(p[3]), float(p[4]), float(p[5]), float(p[7]) if p[7] != "NaN" else float("nan")))
for ph in d:
    if "main" not in d[ph]: continue
    m = [min(r[k] for r in d[ph]["main"]) for k in range(3)]
    ms = statistics.median(r[3] for r in d[ph]["main"])
    for n in d[ph]:
        o = [min(r[k] for r in d[ph][n]) for k in range(3)]
        sc = statistics.median(r[3] for r in d[ph][n])
        print("%-9s %-12s instr %.4f  cycles %.4f  locked %.3f  score %.3f (%.1f)  n=%d" % (ph, n, o[0] / m[0], o[1] / m[1], o[2] / m[2], sc / ms if ms == ms else float('nan'), sc, len(d[ph][n])))
