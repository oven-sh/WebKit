#!/usr/bin/env python3
"""cycles-breakdown-compare.py [dir]: reads breakdown.txt (cycles-breakdown.sh) and prints, per event, the count on main,
each other binary's ratio to main, and for events counted in cycles or slots the difference as a share of main's cycles.
Per binary and event the value is the median over the rounds; the first two lines say how far the rounds of one binary
are apart (cycles, all groups), which is the noise every other line has to be read against."""
import collections, os, statistics, sys
d = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("OUT", "flagoff-out")
v = collections.defaultdict(lambda: collections.defaultdict(list))  # (group,event) -> name -> [counts]
order = []
for l in open(os.path.join(d, "breakdown.txt")):
    p = l.split()
    if len(p) < 7: continue
    ph, n, r, g, e, c, pct = p[:7]
    e = e.replace(":u", "")
    try: c = float(c)
    except ValueError: continue
    k = (g, e)
    if k not in order: order.append(k)
    v[k][n].append(c)
names = sorted({n for k in v for n in v[k]}, key=lambda n: (n != "main", n))
med = lambda x: statistics.median(x)
cyc = {n: med([c for (g, e) in v if e == "cycles" for c in v[(g, e)][n]]) for n in names}
ins = {n: med([c for (g, e) in v if e == "instructions" for c in v[(g, e)][n]]) for n in names}
print("runs per binary: %d; cycles median over all groups and rounds" % len([c for (g, e) in v if e == "cycles" for c in v[(g, e)]["main"]]))
for n in names:
    cs = sorted(c for (g, e) in v if e == "cycles" for c in v[(g, e)][n])
    print("  %-8s cycles %.4e (min %.4f max %.4f of median)  instructions %.4e  ratio to main: cycles %.4f instructions %.4f" % (
        n, cyc[n], cs[0] / cyc[n], cs[-1] / cyc[n], ins[n], cyc[n] / cyc["main"], ins[n] / ins["main"]))
cyc_units = ("stalls", "cycles", "walk_active", "penalty", "bound_on", "exe_bound", "ports_util", "resource_stalls")
print("%-8s %-40s %12s %s" % ("group", "event", "main", "  ".join("%-22s" % (n + " ratio / share") for n in names if n != "main")))
for (g, e) in order:
    if e in ("cycles", "instructions"): continue
    m = med(v[(g, e)]["main"])
    # within the group: normalise by that group's own cycles so that run-to-run drift cancels
    gm = med(v[(g, "cycles")]["main"])
    row = []
    for n in names:
        if n == "main": continue
        x = med(v[(g, e)][n]); gx = med(v[(g, "cycles")][n])
        share = ""
        if any(u in e for u in cyc_units): share = "%+.2f%% cyc" % (100 * (x - m) / gm)
        elif "slots" in e or e == "idq_bubbles.core": share = "%+.2f%% cyc" % (100 * (x - m) / 6 / gm)
        row.append("%-22s" % ("%.4f %s" % (x / m if m else float("nan"), share)))
    print("%-8s %-40s %12.4e %s" % (g, e, m, "  ".join(row)))
