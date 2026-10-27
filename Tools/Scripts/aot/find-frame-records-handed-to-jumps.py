#!/usr/bin/env python3
# find-frame-records-handed-to-jumps.py <binary> [--only <regex of function names>] [--expected <file>] [--objdump <llvm-objdump>]
#
# An operation that compiled code calls publishes the address of its own frame record, and walks of the stack begin there. A C++ compiler is free to end a function with a jump to its last
# callee. Behind such a jump the frame is gone. The callee's own record lies at the same address only where a frame record lies above the registers that a function saves: on x86-64 and on
# Darwin, and not on ARM64 elsewhere. ~AOTOperationPrologueCallFrameTracer() and NOT_TAIL_CALLED keep the compiler from it.
#
# This reads the machine code of a finished ARM64 binary that has its symbols, and lists every function that lets the address of its frame record out (mov xN, x29, or str x29 to anything
# but the stack) and also leaves by a jump to another function.
#
# A function that is listed in --expected (one name a line, '#' starts a comment that says why it is harmless) is not reported. Exit status 1 if anything else is found.
import re, subprocess, sys
a = sys.argv[1:]; binary = a[0]; only = re.compile(r'AOT'); expected = set(); objdump = 'llvm-objdump'
for i, x in enumerate(a):
    if x == '--only': only = re.compile(a[i + 1])
    if x == '--objdump': objdump = a[i + 1]
    if x == '--expected': expected = {l.split('#')[0].strip() for l in open(a[i + 1]) if l.split('#')[0].strip()}
head = re.compile(r'^[0-9a-f]+ <(.*)>:$')
lets_out = re.compile(r'\t(?:mov\tx\d+, x29$|str\tx29, \[x\d+)')
jump = re.compile(r'\tb\t0x[0-9a-f]+ <(.*)>$')
short = lambda name: name.split('(')[0]
name = None; out = []; jumps = set(); functions = 0; found = 0
def finish():
    global found
    if name and out and jumps and only.search(name) and short(name) not in expected:
        found += 1
        print(f'{short(name)}\n    {out[0]}\n    jumps to {", ".join(sorted(short(j) for j in jumps))}')
for line in subprocess.Popen([objdump, '-d', '--no-show-raw-insn', '-C', binary], stdout=subprocess.PIPE, text=True, errors='replace').stdout:
    line = line.rstrip('\n')
    if m := head.match(line):
        finish(); name = m.group(1); out = []; jumps = set(); functions += 1
    elif lets_out.search(line):
        out.append(' '.join(line.split()))
    elif (m := jump.search(line)) and '+0x' not in m.group(1) and m.group(1) != name:
        jumps.add(m.group(1))
finish()
print(f'{functions} functions: {found} found')
sys.exit(1 if found or not functions else 0)
