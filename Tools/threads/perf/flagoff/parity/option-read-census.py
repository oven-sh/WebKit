#!/usr/bin/env python3
"""option-read-census.py <jsc> [<main jsc>]: which bytes of the Config page (options included) the code reads, and in how
many functions, found statically: a `lea g_config(%rip), reg` followed within a few instructions by an access through
that register. With a second binary, the offsets are listed by how many more functions read them than on main: those
are the option tests the branch added, which the per-mode compilation does not fold (they do not read the mode byte).
Offsets differ between the two binaries when the branch added options before them; names come from --names."""
import collections, re, subprocess, sys
def census(binary):
    p = subprocess.Popen(["objdump", "-d", "-C", "--no-show-raw-insn", binary], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, errors="replace")
    fn = None; base = {}  # reg -> instructions left
    reads = collections.defaultdict(set)   # offset -> functions
    sites = collections.Counter()
    for l in p.stdout:
        m = re.match(r'^[0-9a-f]+ <(.*)>:$', l)
        if m: fn = m.group(1); base = {}; continue
        m = re.match(r'^\s*[0-9a-f]+:\t(.*)$', l)
        if not m: continue
        ins = m.group(1)
        g = re.search(r'lea\s+.*\(%rip\),(%\w+)\s+# [0-9a-f]+ <g_config>', ins)
        if g:
            base[g.group(1)] = 12; continue
        g = re.search(r'# [0-9a-f]+ <g_config\+(0x[0-9a-f]+)>', ins)
        if g:
            off = int(g.group(1), 16); reads[off].add(fn); sites[off] += 1
        for r in list(base):
            for a in re.finditer(r'(-?0x[0-9a-f]+)?\(' + re.escape(r) + r'\)', ins):
                off = int(a.group(1), 16) if a.group(1) else 0
                reads[off].add(fn); sites[off] += 1
            base[r] -= 1
            if base[r] <= 0 or re.search(r',' + re.escape(r) + r'$', ins.split('#')[0].strip()): base.pop(r, None)
    return reads, sites
b, bs = census(sys.argv[1])
if len(sys.argv) > 2 and not sys.argv[2].startswith('--'):
    m, ms = census(sys.argv[2])
    print("offset  sites(branch) functions(branch)  [main: sites at the same offset]")
    for off in sorted(bs, key=lambda o: -bs[o])[:60]:
        print("%#6x  %6d %6d   [%d]   e.g. %s" % (off, bs[off], len(b[off]), ms.get(off, 0), sorted(b[off])[0][:90]))
else:
    for off in sorted(bs, key=lambda o: -bs[o])[:60]:
        print("%#6x  %6d %6d   e.g. %s" % (off, bs[off], len(b[off]), sorted(b[off])[0][:90]))
