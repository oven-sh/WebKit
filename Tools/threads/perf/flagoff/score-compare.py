#!/usr/bin/env python3
"""score-compare.py [-v] [-t] [dir]: medians over rounds of the per-test scores in <dir>/js-<round>-<main|off>.txt; the geometric
mean of the ratio flag off / main for the score and for each sub-score (Startup, Worst Case, Average). -v: per-test scores;
-t: per-test sub-scores."""
import collections, glob, math, os, re, statistics, sys
args = [a for a in sys.argv[1:] if not a.startswith("-")]
D = args[0] if args else os.path.join(os.environ.get("OUT", "flagoff-out"), "quiet")
sc = collections.defaultdict(lambda: collections.defaultdict(lambda: collections.defaultdict(list)))
tot = collections.defaultdict(list)
for f in sorted(glob.glob(os.path.join(D, "js-*-*.txt"))):
    cfg = re.match(r"js-\d+-(\w+)\.txt", os.path.basename(f)).group(1); cur = None; done = False
    for line in open(f, errors="replace"):
        m = re.match(r"^Running (.+):", line)
        if m: cur = m.group(1); continue
        m = re.match(r"^\s+(\w[\w ]*?):\s+([\d.]+)", line)
        if m and cur: sc[cfg][cur][m.group(1)].append(float(m.group(2)))
        m = re.match(r"Total Score:\s+([\d.]+)", line)
        if m: tot[cfg].append(float(m.group(1))); done = True
    if not done: print("incomplete:", f, file=sys.stderr)
def g(x): return math.exp(sum(math.log(v) for v in x) / len(x)) if x else float("nan")
med = statistics.median
tests = sorted(sc["main"])
print("rounds:", {c: len(tot[c]) for c in ("main", "off")})
for c in ("main", "off"):
    if tot[c]: print("total score, median: %-5s %.2f  (min %.2f max %.2f)" % (c, med(tot[c]), min(tot[c]), max(tot[c])))
keys = ["Score", "First", "Startup", "Worst Case", "Worst", "Average"]
print("%-11s %8s %5s" % ("flag off/main", "ratio", "n"))
for k in keys:
    r = []
    for t in tests:
        a = sc["main"][t].get(k); b = sc["off"].get(t, {}).get(k)
        if a and b and med(a) > 0: r.append(med(b) / med(a))
    if r: print("%-11s %8.3f %5d" % (k, g(r), len(r)))
if "-v" in sys.argv or "-t" in sys.argv:
    print("%-28s" % "test" + "".join("%11s" % k[:10] for k in keys))
    for t in tests:
        row = []
        for k in keys:
            a = sc["main"][t].get(k); b = sc["off"].get(t, {}).get(k)
            row.append("%11.3f" % (med(b) / med(a)) if a and b else "%11s" % "-")
        print("%-28s" % t + "".join(row))
