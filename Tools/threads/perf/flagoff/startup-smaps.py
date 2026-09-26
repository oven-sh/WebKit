#!/usr/bin/env python3
"""startup-smaps.py <main jsc> <branch jsc>: resident memory of a shell that has started and waits (readline()), by
mapping, main against the branch: where the start-up's resident set differs. Rss and private dirty in kB."""
import collections, os, re, subprocess, sys, time
def snap(jsc):
    env = {k: v for k, v in os.environ.items() if not k.startswith("JSC_")}
    p = subprocess.Popen([jsc, "-e", "readline()"], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    time.sleep(0.5)
    text = open("/proc/%d/smaps" % p.pid).read()
    p.stdin.close(); p.wait()
    kinds = collections.defaultdict(lambda: [0, 0, 0])
    hdr = re.compile(r"^([0-9a-f]+)-([0-9a-f]+) (\S+) \S+ \S+ \S+\s*(.*)$")
    cur = None
    def flush(c):
        if not c: return
        name, perms, size = c["name"], c["perms"], c.get("Size", 0)
        if name.startswith("/"): k = "file %s %s" % (re.sub(r"[-.][0-9a-f.]+$", "", name.rsplit("/", 1)[-1])[:28], perms[:3])
        elif name: k = name
        elif "x" in perms: k = "anon executable"
        else: k = "anon %s %s" % (perms[:3], "<1MB" if size < 1024 else "<16MB" if size < 16384 else "<256MB" if size < 262144 else ">=256MB")
        kinds[k][0] += c.get("Rss", 0); kinds[k][1] += c.get("Private_Dirty", 0); kinds[k][2] += 1
    for line in text.split("\n"):
        m = hdr.match(line)
        if m:
            flush(cur); cur = {"perms": m.group(3), "name": m.group(4).strip()}
        elif cur and ":" in line:
            k, v = line.split(":", 1)
            if k in ("Rss", "Size", "Private_Dirty"): cur[k] = int(v.split()[0])
    flush(cur)
    return kinds
def norm(k, names):
    for n in names: k = k.replace(n, "jsc")
    return k
a = snap(sys.argv[1]); b = snap(sys.argv[2])
names = [os.path.basename(sys.argv[1])[:28], os.path.basename(sys.argv[2])[:28]]
a = {norm(k, names): v for k, v in a.items()}; b = {norm(k, names): v for k, v in b.items()}
print("%-40s %10s %10s %8s   %10s %10s" % ("mapping", "main rss", "branch", "diff", "main dirty", "branch"))
ta = tb = 0
for k in sorted(set(a) | set(b), key=lambda k: -abs(b.get(k, [0])[0] - a.get(k, [0])[0])):
    x = a.get(k, [0, 0, 0]); y = b.get(k, [0, 0, 0]); ta += x[0]; tb += y[0]
    if x[0] or y[0]: print("%-40s %10d %10d %+8d   %10d %10d" % (k, x[0], y[0], y[0] - x[0], x[1], y[1]))
print("%-40s %10d %10d %+8d" % ("total", ta, tb, tb - ta))
