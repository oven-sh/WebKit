#!/usr/bin/env python3
"""cgdiff.py <A.fn> <B.fn> [N]: per-function exclusive instruction counts, B - A, largest first. Several pairs may be summed:
cgdiff.py -sum <dirA> <dirB> [N] sums every *.fn of each directory."""
import sys, collections, glob, os, re
def load(paths):
    d = collections.Counter()
    for p in paths:
        for l in open(p, errors='replace'):
            c, _, f = l.rstrip('\n').partition('\t')
            f = re.sub(r"'\d+$", '', f.strip())
            f = re.sub(r'^0x[0-9a-f]+$', '[unknown]', f)
            f = re.sub(r'^(?:bool|void|JSC::JSValue|JSC::UGPRPair|JSC::EncodedJSValue|[A-Za-z_:<>*& ]+?) (?=[A-Za-z_:]+PerThreadsMode<)', '', f) if 'PerThreadsMode<' in f else f
            f = f.replace('PerThreadsMode<false>', '').replace('PerThreadsMode<true>', '[threaded]')
            m = re.match(r'^JSC::(?:LLInt::)?((?:llint_)?slow_path_\w+|llint_\w+)\(JSC::CallFrame\*.*\)$', f)
            if m: f = m.group(1)
            f = re.sub(r'visitButterflyForMode<([^,]+), false>', r'visitButterflyImpl<\1>', f)
            if c.isdigit() and f != "PROGRAM TOTALS": d[f] += int(c)
    return d
args = sys.argv[1:]
if args[0] == '-sum':
    common = sorted(set(os.path.basename(f) for f in glob.glob(os.path.join(args[1], '*.fn'))) & set(os.path.basename(f) for f in glob.glob(os.path.join(args[2], '*.fn'))))
    a = load([os.path.join(args[1], f) for f in common]); b = load([os.path.join(args[2], f) for f in common]); rest = args[3:]
    print("%d tests in both" % len(common))
else:
    a = load([args[0]]); b = load([args[1]]); rest = args[2:]
n = int(rest[0]) if rest else 40
ta = sum(a.values()); tb = sum(b.values())
print("total A %d  B %d  B/A %.4f  (B-A %+d)" % (ta, tb, tb / ta, tb - ta))
diff = sorted(((b[f] - a[f], f) for f in set(a) | set(b)), reverse=True)
for d, f in diff[:n]: print("%+12d %6.3f%%  A %11d  B %11d  %s" % (d, 100.0 * d / ta, a[f], b[f], f[:140]))
print("   ...")
for d, f in diff[-12:]: print("%+12d %6.3f%%  A %11d  B %11d  %s" % (d, 100.0 * d / ta, a[f], b[f], f[:140]))
