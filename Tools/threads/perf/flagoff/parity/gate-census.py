#!/usr/bin/env python3
"""gate-census.py <jsc> [symbol=g_jscThreadsModePage]: for every function of the binary, the number of instructions that address the
process-mode byte (the static gate tests), with the function's size. Output: TSV 'gates size symbol', most gates first."""
import subprocess, sys, re, collections
jsc = sys.argv[1]; sym = sys.argv[2] if len(sys.argv) > 2 else "g_jscThreadsModePage"
addr = None
for l in subprocess.run(["nm", jsc], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 3 and p[2] == sym: addr = int(p[0], 16)
if addr is None: sys.exit("no symbol " + sym)
off = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0
target = "%x" % (addr + off)
pat = re.compile(r"#\s*(0x)?%s\b" % target)
cur = None; start = 0; last = 0
gates = collections.Counter(); size = {}
p = subprocess.Popen(["objdump", "-d", "--no-show-raw-insn", "-j", ".text", jsc], stdout=subprocess.PIPE, text=True, errors="replace")
hdr = re.compile(r"^([0-9a-f]+) <(.*)>:$")
for l in p.stdout:
    m = hdr.match(l)
    if m:
        if cur is not None: size[cur] = size.get(cur, 0) + last - start
        cur = m.group(2); start = int(m.group(1), 16); last = start; continue
    if cur is None or not l.startswith(" "): continue
    try: last = int(l.split(":", 1)[0].strip(), 16)
    except ValueError: continue
    if pat.search(l): gates[cur] += 1
if cur is not None: size[cur] = size.get(cur, 0) + last - start
tot = sum(gates.values())
sys.stderr.write("target %s: %d gate instructions in %d functions of %d\n" % (target, tot, len(gates), len(size)))
for s, n in gates.most_common():
    print("%d\t%d\t%s" % (n, size.get(s, 0), s))
