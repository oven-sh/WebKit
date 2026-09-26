#!/usr/bin/env python3
"""Interleaved runs of several configurations of one command, with per-thread CPU time and RSS.
usage: multi.py <rounds> <out.json> <config-file>
config-file: one configuration per line: name<TAB>command line
The order inside a round rotates, so that no configuration always runs first.
The first configuration runs twice in each round (A and A'): the A/A pair is the noise floor."""
import json, os, statistics, subprocess, sys, shlex

rounds = int(sys.argv[1]); out_path = sys.argv[2]
configs = [l.rstrip("\n").split("\t", 1) for l in open(sys.argv[3]) if l.strip() and not l.startswith("#")]
configs.insert(1, [configs[0][0] + " (A')", configs[0][1]])
RUNSTAT = "/workspace/wkbuild/tools/runstat"
results = {name: [] for name, _ in configs}
for r in range(rounds):
    order = configs[r % len(configs):] + configs[:r % len(configs)]
    for name, cmd in order:
        tmp = "/tmp/multi-%d.txt" % os.getpid()
        if os.path.exists(tmp): os.remove(tmp)
        env = dict(os.environ, RUNSTAT_OUT=tmp)
        p = subprocess.run([RUNSTAT] + shlex.split(cmd), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        line = open(tmp).read().strip().splitlines()[-1]
        d = {k: float(v) for k, v in (kv.split("=") for kv in line.split())}
        d["ok"] = p.returncode == 0
        results[name].append(d)
        print("round %d %-34s main_user=%.2f jit=%.2f gc=%.2f rss=%.0fMB wall=%.2f%s" % (r, name, d["main_user"], d["jit_cpu"], d["gc_cpu"], d["maxrss_kb"] / 1024, d["wall"], "" if d["ok"] else " FAILED"), flush=True)
json.dump(results, open(out_path, "w"), indent=1)
base = configs[0][0]
def med(name, key): return statistics.median(x[key] for x in results[name])
print()
print("%-34s %22s %22s %22s %20s" % ("medians of %d runs" % rounds, "main thread user s", "JIT threads CPU s", "main+JIT+GC CPU s", "max RSS MB"))
for name, _ in configs:
    cells = []
    for key, scale in (("main_user", 1), ("jit_cpu", 1), (None, 1), ("maxrss_kb", 1 / 1024)):
        if key is None:
            vals = [x["main_user"] + x["main_sys"] + x["jit_cpu"] + x["gc_cpu"] for x in results[name]]
            basevals = [x["main_user"] + x["main_sys"] + x["jit_cpu"] + x["gc_cpu"] for x in results[base]]
        else:
            vals = [x[key] * scale for x in results[name]]
            basevals = [x[key] * scale for x in results[base]]
        m = statistics.median(vals); b = statistics.median(basevals)
        cells.append("%8.2f %+6.1f%% [%0.2f..%0.2f]" % (m, (m - b) / b * 100 if b else 0, min(vals), max(vals)) if key != "maxrss_kb" else "%7.0f %+6.1f%%" % (m, (m - b) / b * 100))
    print("%-34s %s" % (name, "  ".join(cells)))
