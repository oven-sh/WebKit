#!/usr/bin/env python3
"""lock-census.py <main jsc> <branch jsc> [N] : the functions in which the branch's binary has more locked instructions (a `lock` prefix or
xchg with memory) than main's binary has in the function of the same name; a process without the flag runs them as they are. Static, so
it does not say how often; profile.sh's locked-load samples do. Prints the N (default 60) largest excesses."""
import collections, re, subprocess, sys
def census(binary):
    p = subprocess.Popen(["objdump", "-d", "--no-show-raw-insn", "-C", "-j", ".text", binary], stdout=subprocess.PIPE, text=True, errors="replace", bufsize=1 << 20)
    cur = None; d = collections.Counter(); size = collections.Counter()
    for l in p.stdout:
        if l.endswith(">:\n") and l[0].isdigit():
            cur = l.split("<", 1)[1].rsplit(">", 1)[0]
        elif cur is not None and "\t" in l:
            size[cur] += 1
            if "\tlock " in l or re.search(r"\txchg\s.*\(", l): d[cur] += 1
    return d, size
a, sa = census(sys.argv[1]); b, sb = census(sys.argv[2])
rows = []
for k in b:
    if b[k] > a.get(k, 0): rows.append((b[k] - a.get(k, 0), b[k], a.get(k, 0), k))
rows.sort(reverse=True)
print("functions with more locked instructions than main: %d; total locked instructions main %d branch %d" % (len(rows), sum(a.values()), sum(b.values())))
for ex, nb, na, k in rows[:int(sys.argv[3]) if len(sys.argv) > 3 else 60]: print("%+4d  main %3d branch %3d  %s" % (ex, na, nb, k[:150]))
