#!/usr/bin/env python3
"""option-exec.py <jsc with debug information> <callgrind out file made with --dump-instr=yes> [rows]: how many times the
run executed an instruction that reads one of the options the branch added (OptionsList.h from useJSThreads to the end
of its block, and the Config bytes after the options), per option and per function. gate-exec.py counts the tests of
the threads-mode byte, which the per-mode compilation folds; these tests read the options themselves and are not folded.
The callgrind file may come from a stripped copy of the same binary."""
import collections, os, re, subprocess, sys, tempfile
jsc, cg = sys.argv[1], sys.argv[2]
rows = int(sys.argv[3]) if len(sys.argv) > 3 else 60
GDB = r'''
import gdb, os
def off(t, name):
    for f in t.fields():
        if f.name == name: return f.bitpos // 8
w = gdb.lookup_type("WTF::Config").strip_typedefs()
j = gdb.lookup_type("JSC::Config").strip_typedefs()
o = gdb.lookup_type("JSC::OptionsStorage").strip_typedefs()
base = off(w, "spaceForExtensions"); ob = off(j, "options")
out = open(os.environ["OPTION_EXEC_OUT"], "w")
for f in j.fields():
    if hasattr(f, "bitpos") and f.name != "options": out.write("%d\tConfig::%s\n" % (base + f.bitpos // 8, f.name))
for f in o.fields():
    if hasattr(f, "bitpos"): out.write("%d\t%s\n" % (base + ob + f.bitpos // 8, f.name))
out.close()
'''
with tempfile.TemporaryDirectory() as d:
    open(d + "/s.py", "w").write(GDB)
    subprocess.run(["gdb", "-q", "-batch", "-x", d + "/s.py", jsc], env=dict(os.environ, OPTION_EXEC_OUT=d + "/o.txt"), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    fields = [l.rstrip("\n").split("\t") for l in open(d + "/o.txt")]
syms = {}
for l in subprocess.run(["nm", jsc], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 3: syms[p[2]] = int(p[0], 16)
# The Config page's symbol and the struct's start differ by the page's header: locate the struct through a field the
# interpreter reads by a fixed name.
gconfig = syms.get("g_config") or syms.get("WebConfig::g_config")
names = {}
order = [n for _, n in fields]
first = order.index("useJSThreads")
last = order.index("Config::gilOffProcess") if "Config::gilOffProcess" in order else len(order) - 1
wanted = set(order[first:]) | {"Config::gilOffProcess", "Config::butterflyTIDTagTLSKey"}
header = int(os.environ.get("CONFIG_HEADER", "0x1a0"), 16)
for o, n in fields:
    if n in wanted: names[int(o) + header] = n
gates = {}
cur = None; base = {}
hdr = re.compile(r"^([0-9a-f]+) <(.*)>:$")
p = subprocess.Popen(["objdump", "-d", "--no-show-raw-insn", "-C", "-j", ".text", jsc], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, errors="replace")
for l in p.stdout:
    m = hdr.match(l)
    if m: cur = m.group(2); base = {}; continue
    m = re.match(r"^\s*([0-9a-f]+):\t(.*)$", l)
    if not m or not cur: continue
    a = int(m.group(1), 16); ins = m.group(2)
    g = re.search(r"lea\s+.*\(%rip\),(%\w+)\s+# [0-9a-f]+ <g_config>", ins)
    if g:
        base[g.group(1)] = 16; continue
    g = re.search(r"# [0-9a-f]+ <g_config\+(0x[0-9a-f]+)>", ins)
    if g and int(g.group(1), 16) in names:
        gates[a] = (cur, names[int(g.group(1), 16)])
    for r in list(base):
        for x in re.finditer(r"(-?0x[0-9a-f]+)\(" + re.escape(r) + r"\)", ins):
            off = int(x.group(1), 16)
            if off in names: gates[a] = (cur, names[off])
        base[r] -= 1
        code = ins.split("#")[0].strip()
        if base[r] <= 0 or (code.endswith("," + r) and not code.startswith(("cmp", "test"))): base.pop(r, None)
sys.stderr.write("%d instructions read one of %d options\n" % (len(gates), len(names)))
byopt = collections.Counter(); byfn = collections.Counter(); total = 0
obnames = {}; pos = 0; incall = False; npos = 1; inmain = True
for l in open(cg, errors="replace"):
    if not l or l[0] in "#\n": continue
    if l[0].isalpha():
        if l.startswith("calls="): incall = True
        elif l.startswith("positions:"): npos = len(l.split()) - 1
        elif l.startswith(("cob=", "ob=")):
            m = re.match(r"c?ob=\((\d+)\)(?: (.*))?", l.strip())
            if m and m.group(2): obnames[m.group(1)] = m.group(2)
            if l.startswith("ob="): inmain = bool(m) and "jsc" in obnames.get(m.group(1), "").rsplit("/", 1)[-1]
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
    g = gates.get(pos) if inmain else None
    if g:
        byfn[g] += n; byopt[g[1]] += n
s = sum(byopt.values())
print("instructions %d, option reads executed %d (%.3f%%)" % (total, s, 100.0 * s / max(total, 1)))
for o, n in byopt.most_common(): print("%12d %.4f%%  %s" % (n, 100.0 * n / total, o))
print("by function:")
for (f, o), n in byfn.most_common(rows): print("%12d %.4f%%  %-32s %s" % (n, 100.0 * n / total, o, f[:150]))
