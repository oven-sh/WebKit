#!/usr/bin/env python3
"""lint-llint-flagoff-bodies.py <LLIntAssembly.h> : the interpreter bodies a process without useJSThreads runs -- the labels
llint_op_<name> and their _wide16/_wide32 forms, for the opcodes LowLevelInterpreter.asm installs threaded twins of -- must contain no
test of the flag or of the tagged-butterfly option: every one of them is `main`'s body. An assembler line of such a body that comes
from an asm source line naming useJSThreads, useTaggedButterflies or gilOffProcess is a finding, except the Group-3 discriminator
(`gilOffGroup3Check`: one test of the process byte where the VM's state is read after a call into C++; kept by design, FLAG-OFF-LANDING
L5.9). The threaded twins (threaded_llint_*) are not looked at. Exit 0 when there is no finding."""
import re, sys, os, collections

gen = sys.argv[1]
root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
llint = os.path.join(root, "Source/JavaScriptCore/llint")
asm = {f: open(os.path.join(llint, f), errors="replace").read().split("\n") for f in ("LowLevelInterpreter.asm", "LowLevelInterpreter64.asm")}
entries = open(os.path.join(llint, "LowLevelInterpreter.asm")).read()
ops = sorted(set(re.findall(r"^\s*setThreadedEntries\(Op\w+, (\w+)\)", entries, re.M)))

# source lines of the Group-3 discriminator macro
lo = hi = None
for i, l in enumerate(asm["LowLevelInterpreter.asm"]):
    if l.startswith("macro gilOffGroup3Check("): lo = i + 1
    elif lo and hi is None and l.strip() == "end": hi = i + 1
GATE = re.compile(r"useJSThreads|useTaggedButterflies|gilOffProcess|ifJSThreadsBranch|ifTaggedButterfliesBranch")

lines = open(gen, errors="replace").read().split("\n")
starts = []
for i, l in enumerate(lines):
    m = re.match(r"OFFLINE_ASM_OPCODE_LABEL\((op_\w+)\)", l) or re.match(r"OFFLINE_ASM_GLUE_LABEL\((\w+)\)", l)
    if not m:
        t = re.match(r"OFFLINE_ASM_THREADED_OPCODE_LABEL\((op_\w+)\)", l)
        if t: m = re.match(r"(.*)", "threaded_llint_" + t.group(1))
    if m: starts.append((i, m.group(1)))
idx = {n: i for i, n in starts}; order = [i for i, _ in starts]
findings = collections.defaultdict(set); checked = 0
names = [op + suf for op in ops for suf in ("", "_wide16", "_wide32")]
if "--all" in sys.argv:  # every opcode label of the flag-off interpreter, not only the twinned ones
    names = [n for _, n in starts if n.startswith("op_")]
for name in names:
    if True:
        if name not in idx:
            print("missing label", name); sys.exit(2)
        checked += 1
        s = idx[name]; e = min([x for x in order if x > s] or [len(lines)])
        for l in lines[s:e]:
            m = re.search(r"// (LowLevelInterpreter\w*\.asm):(\d+)", l)
            # a compare of a byte or a load of the Config page; the source line of an instruction that follows a gate that emitted
            # nothing can be the gate's own
            if not m or not re.search(r'"(cmpb|testb|cmpl \$0|movq " LABEL_REFERENCE\(g_config\))', l): continue
            f, n = m.group(1), int(m.group(2))
            if f == "LowLevelInterpreter.asm" and lo and lo <= n <= hi: continue
            src = asm[f][n - 1] if n - 1 < len(asm[f]) else ""
            if GATE.search(src): findings[name].add((f, n, src.strip()[:90]))
for name in sorted(findings):
    for f, n, src in sorted(findings[name]): print("%s: %s:%d: %s" % (name, f, n, src))
print("flag-off interpreter bodies: %d checked, %d with a gate test" % (checked, len(findings)))
sys.exit(1 if findings else 0)
