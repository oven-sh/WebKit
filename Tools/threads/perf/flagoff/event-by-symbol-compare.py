#!/usr/bin/env python3
"""event-by-symbol-compare.py <dir> <event> <name> [rows]: the symbols with the largest difference in samples between
<name> and main (event-by-symbol.sh). Per-mode copies (…PerThreadsMode<false>, the lambdas of the in-place form) are
folded into the function they belong to, so that a function compares with itself."""
import collections, re, sys
d, ev, name = sys.argv[1:4]; rows = int(sys.argv[4]) if len(sys.argv) > 4 else 40
def norm(s):
    s = re.sub(r'PerThreadsMode<(false|true)>', '', s)
    s = re.sub(r'::\{lambda\(\)#\d+\}::operator\(\)<(false|true)>\(\) const', '', s)
    s = re.sub(r'\$_\d+::operator\(\)<(false|true)>', '', s)
    s = re.sub(r'(Body|Impl)<(false|true)>', r'\1', s)
    return s.strip()
def load(n):
    a = collections.Counter()
    for l in open("%s/%s-%s.txt" % (d, ev, n)):
        c, s = l.rstrip("\n").split("\t", 1); a[norm(s)] += int(c)
    return a
m = load("main"); b = load(name)
tm = sum(m.values()); tb = sum(b.values())
print("%s: main %d samples, %s %d (%.4f)" % (ev, tm, name, tb, tb / tm))
keys = sorted(set(m) | set(b), key=lambda k: -(b[k] - m[k]))
print("largest increases:")
for k in keys[:rows]: print("  %+7d  %7d -> %7d  %s" % (b[k] - m[k], m[k], b[k], k[:130]))
print("largest decreases:")
for k in keys[-10:]: print("  %+7d  %7d -> %7d  %s" % (b[k] - m[k], m[k], b[k], k[:130]))
