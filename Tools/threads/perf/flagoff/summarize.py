#!/usr/bin/env python3
"""summarize.py <results dir>...: the L5 table of FLAG-OFF-LANDING for each results directory of baseline.sh (flag off / main): first
iteration, interpreter only, Baseline- and DFG-capped, whole run (cycles, all main thread), locked loads, the quiet pass with its
sub-scores, start-up (instructions, resident set of an empty script), and the peak resident set of the suite in one process."""
import collections, glob, math, os, re, statistics, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
def geomean(xs): return math.exp(sum(math.log(x) for x in xs) / len(xs))
def phases(d):
    rows = collections.defaultdict(lambda: collections.defaultdict(list))
    for l in open(os.path.join(d, "phases.txt")):
        p = l.split()
        if len(p) < 7: continue
        rows[p[0]][p[1]].append((float(p[3]), float(p[4]), float(p[5])))
    out = {}
    for ph, v in rows.items():
        if "main" in v and "off" in v:
            f = lambda i: statistics.median([x[i] for x in v["off"]]) / statistics.median([x[i] for x in v["main"]])
            out[ph] = (f(1), f(0), f(2))
    return out
def quiet(d):
    o = subprocess.run([sys.executable, os.path.join(HERE, "score-compare.py"), os.path.join(d, "quiet")], capture_output=True, text=True).stdout
    r = {}
    for l in o.split("\n"):
        m = re.match(r"(Score|Startup|Worst Case|Average)\s+([0-9.]+)", l)
        if m: r[m.group(1)] = float(m.group(2))
    return r
def counters(d):
    o = subprocess.run([sys.executable, os.path.join(HERE, "counters-compare.py"), os.path.join(d, "counters.txt")], capture_output=True, text=True).stdout
    m = {}
    for l in o.split("\n"):
        p = l.split()
        if len(p) >= 6 and p[0] in ("main", "off"): m[p[0]] = [float(x) for x in p[1:6]]
    return m
def startup(d):
    v = {}
    for l in open(os.path.join(d, "startup.txt")):
        p = l.split()
        if len(p) >= 5: v[(p[0], p[1])] = (float(p[2]), float(p[4]))
    return v
def rss(d):
    f = os.path.join(d, "memory.txt")
    if not os.path.exists(f): return None
    a = collections.defaultdict(list)
    for l in open(f):
        t, c, r, kb = l.split()
        if t == "ALL": a[c].append(int(kb))
    return (statistics.median(a["off"]) / statistics.median(a["main"])) if a["main"] and a["off"] else None
for d in sys.argv[1:]:
    ph = phases(d); q = quiet(d); c = counters(d); s = startup(d); r = rss(d)
    print("== %s" % os.path.basename(d.rstrip("/")))
    if "first" in ph: print("  first iteration, main-thread cycles:        %.3f  (instr %.3f, locked loads %.3f)" % ph["first"])
    if "cap-useJIT" in ph: print("  interpreter only (--useJIT=0), cycles:      %.3f  (instr %.3f)" % ph["cap-useJIT"][:2])
    if "cap-useDFGJIT" in ph: print("  Baseline-capped (--useDFGJIT=0), cycles:    %.3f" % ph["cap-useDFGJIT"][0])
    if "cap-useFTLJIT" in ph: print("  DFG-capped (--useFTLJIT=0), cycles:         %.3f" % ph["cap-useFTLJIT"][0])
    if "full" in ph: print("  whole run, main-thread cycles:              %.3f  (instr %.3f, locked loads %.3f)" % ph["full"])
    if c: print("  per-test counters (min of runs), cycles:    %.3f  (instr %.3f, locked loads per k instr %.3f vs %.3f)" % (c["off"][1] / c["main"][1], c["off"][0] / c["main"][0], c["off"][4], c["main"][4]))
    if q: print("  quiet pass: Startup %.3f  Worst Case %.3f  Average %.3f  score %.3f" % (q.get("Startup", 0), q.get("Worst Case", 0), q.get("Average", 0), q.get("Score", 0)))
    if ("empty", "off") in s: print("  start-up, empty script: instructions +%.2f %%, peak resident +%.0f kB" % (100 * (s[("empty", "off")][0] / s[("empty", "main")][0] - 1), s[("empty", "off")][1] - s[("empty", "main")][1]))
    if r: print("  peak resident set, suite in one process (median): %.3f" % r)
