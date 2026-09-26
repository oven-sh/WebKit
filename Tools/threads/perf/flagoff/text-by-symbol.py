#!/usr/bin/env python3
"""text-by-symbol.py <main jsc> <branch jsc> [rows]: where the branch's text is larger: the bytes of symbols `main` does not
have, the growth of symbols both have, and the copies compiled per threads mode, with the largest of each. Sizes are of
the text symbols (nm -S), summed per demangled name; the section sizes come from `size -A`."""
import collections, re, subprocess, sys
def syms(b):
    a = collections.Counter()
    for l in subprocess.run(["nm", "-S", "--size-sort", "-C", b], capture_output=True, text=True).stdout.splitlines():
        p = l.split(" ", 3)
        if len(p) == 4 and p[2] in "tTwW": a[p[3]] += int(p[1], 16)
    return a
def text(b):
    for l in subprocess.run(["size", "-A", b], capture_output=True, text=True).stdout.splitlines():
        p = l.split()
        if p and p[0] == ".text": return int(p[1])
    return 0
m, b = syms(sys.argv[1]), syms(sys.argv[2]); rows = int(sys.argv[3]) if len(sys.argv) > 3 else 15
tm, tb = text(sys.argv[1]), text(sys.argv[2])
print(".text: main %d, branch %d (%+d, %+.1f %%)" % (tm, tb, tb - tm, 100.0 * (tb - tm) / tm))
new = {k: v for k, v in b.items() if k not in m}
gone = {k: v for k, v in m.items() if k not in b}
grown = {k: b[k] - m[k] for k in b if k in m and b[k] != m[k]}
permode = {k: v for k, v in new.items() if re.search(r"PerThreadsMode<|operator\(\)<(true|false)>|Body<(true|false)>", k)}
print("symbols only the branch has: %d bytes in %d symbols (of which copies per threads mode: %d bytes in %d)" % (sum(new.values()), len(new), sum(permode.values()), len(permode)))
print("symbols only main has:       %d bytes in %d symbols" % (sum(gone.values()), len(gone)))
print("symbols both have:           %+d bytes (%d grew by %d, %d shrank by %d)" % (sum(grown.values()), sum(1 for v in grown.values() if v > 0), sum(v for v in grown.values() if v > 0), sum(1 for v in grown.values() if v < 0), -sum(v for v in grown.values() if v < 0)))
print("largest growth:")
for k in sorted(grown, key=lambda k: -grown[k])[:rows]: print("  %+8d  %8d -> %8d  %s" % (grown[k], m[k], b[k], k[:120]))
print("largest new symbols:")
for k in sorted(new, key=lambda k: -new[k])[:rows]: print("  %8d  %s" % (new[k], k[:130]))
