#!/usr/bin/env python3
"""incdiff.py <main.inc> <branch.inc> [regex] [rows]: inclusive instruction counts (a function with everything it calls) per
function, branch minus main, for the functions whose name matches the regex. Per-mode copies are folded into their function."""
import collections, re, sys
def norm(s):
    s = re.sub(r"'\d+$", "", s)
    s = re.sub(r'PerThreadsMode<(false|true)>', '', s)
    s = re.sub(r'::\$_\d+::operator\(\)<(false|true)>\(\) const', '', s)
    s = re.sub(r'::\{lambda\(\)#\d+\}::operator\(\)<(false|true)>\(\) const', '', s)
    s = re.sub(r'Body<(false|true)>', '', s)
    return s.strip()
def load(p):
    a = collections.Counter()
    for l in open(p, errors='replace'):
        c, s = l.rstrip('\n').split('\t', 1)
        n = norm(s)
        a[n] = max(a[n], int(c))  # a wrapper and its copy: the larger is the whole
    return a
m = load(sys.argv[1]); b = load(sys.argv[2])
rx = re.compile(sys.argv[3]) if len(sys.argv) > 3 else re.compile('.')
rows = int(sys.argv[4]) if len(sys.argv) > 4 else 50
tot = max(m.values())
keys = [k for k in set(m) | set(b) if rx.search(k)]
keys.sort(key=lambda k: -(b[k] - m[k]))
print("total (largest inclusive count on main): %.4e" % tot)
for k in keys[:rows]:
    print("%+11d %+.3f%%  %12d -> %12d (%.3f)  %s" % (b[k] - m[k], 100.0 * (b[k] - m[k]) / tot, m[k], b[k], (b[k] / m[k]) if m[k] else float('nan'), k[:120]))
