#!/usr/bin/env python3
"""gate-exec.py <stripped jsc> <callgrind out file made with --dump-instr=yes>: how many times each function executed an instruction that
addresses the threads-mode page (a gate test), most first."""
import sys, re, subprocess, collections
jsc, cg = sys.argv[1], sys.argv[2]
addr = None
for l in subprocess.run(["nm", jsc], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 3 and p[2] == "g_jscThreadsModePage": addr = int(p[0], 16)
gates = {}
obnames = {}
cur = None
pat = re.compile(r"#\s*(0x)?%x\b" % addr)
hdr = re.compile(r"^([0-9a-f]+) <(.*)>:$")
p = subprocess.Popen(["objdump", "-d", "--no-show-raw-insn", "-C", "-j", ".text", jsc], stdout=subprocess.PIPE, text=True, errors="replace")
for l in p.stdout:
    m = hdr.match(l)
    if m: cur = m.group(2); continue
    if cur and pat.search(l):
        try: gates[int(l.split(":", 1)[0].strip(), 16)] = cur
        except ValueError: pass
sys.stderr.write("%d gate instructions\n" % len(gates))
count = collections.Counter(); total = 0
pos = 0; incall = False; npos = 1; inmain = True
for l in open(cg, errors="replace"):
    if not l or l[0] in "#\n": continue
    c = l[0]
    if c.isalpha():
        if l.startswith("calls="): incall = True
        elif l.startswith("positions:"): npos = len(l.split()) - 1
        elif l.startswith("cob="):
            m = re.match(r"cob=\((\d+)\)(?: (.*))?", l.strip())
            if m and m.group(2): obnames[m.group(1)] = m.group(2)
        elif l.startswith("ob="):
            m = re.match(r"ob=\((\d+)\)(?: (.*))?", l.strip())
            if m and m.group(2): obnames[m.group(1)] = m.group(2)
            inmain = bool(m) and "jsc" in obnames.get(m.group(1), "").rsplit("/", 1)[-1]
        continue
    parts = l.split()
    if len(parts) < npos + 1: continue
    a = parts[0]
    if a.startswith("0x"): pos = int(a, 16)
    elif a[0] == "+": pos += int(a[1:], 16) if a[1:].startswith("0x") else int(a[1:])
    elif a[0] == "-": pos -= int(a[1:], 16) if a[1:].startswith("0x") else int(a[1:])
    elif a == "*": pass
    else:
        try: pos = int(a, 16)
        except ValueError: continue
    if incall:
        incall = False; continue
    try: n = int(parts[npos])
    except ValueError: continue
    total += n
    f = gates.get(pos) if inmain else None
    if f: count[f] += n
g = sum(count.values())
print("instructions %d, gate instructions executed %d (%.3f%%)" % (total, g, 100.0 * g / max(total, 1)))
for f, n in count.most_common(int(sys.argv[3]) if len(sys.argv) > 3 else 60):
    print("%12d %.3f%%  %s" % (n, 100.0 * n / total, f[:170]))
