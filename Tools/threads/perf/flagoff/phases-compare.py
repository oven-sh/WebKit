#!/usr/bin/env python3
"""phases-compare.py [dir]: flag off / main for each phase of phases.sh (minima over rounds): main-thread instructions,
cycles, locked loads. The four cycle metrics of FLAG-OFF-LANDING L5 are 'first', 'cap-useJIT' (LLInt only), and 'full'."""
import collections, math, os, sys
D = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("OUT", "flagoff-out")
d = collections.defaultdict(lambda: collections.defaultdict(list))
for l in open(os.path.join(D, "phases.txt")):
    p = l.split()
    if len(p) < 8 or not p[3].isdigit(): continue
    d[p[0]][p[1]].append((float(p[3]), float(p[4]), float(p[5])))
print("%-14s %8s %8s %10s   runs(main/off)" % ("phase", "instr", "cycles", "lockedld"))
for ph in ("full", "first", "six", "cap-useJIT", "cap-useDFGJIT", "cap-useFTLJIT"):
    if ph not in d or "main" not in d[ph] or "off" not in d[ph]: continue
    m = [min(r[k] for r in d[ph]["main"]) for k in range(3)]; o = [min(r[k] for r in d[ph]["off"]) for k in range(3)]
    print("%-14s %8.4f %8.4f %10.3f   %d/%d" % (ph, o[0] / m[0], o[1] / m[1], o[2] / m[2], len(d[ph]["main"]), len(d[ph]["off"])))
rss = collections.defaultdict(list)
p = os.path.join(D, "rss.txt")
if os.path.exists(p):
    for l in open(p):
        w = l.split()
        if len(w) == 2 and w[1].isdigit(): rss[w[0]].append(int(w[1]))
    if rss["main"] and rss["off"]: print("peak RSS (whole run, MB): main %s  off %s" % ([v // 1024 for v in rss["main"]], [v // 1024 for v in rss["off"]]))
