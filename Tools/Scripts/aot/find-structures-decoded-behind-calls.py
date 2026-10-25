#!/usr/bin/env python3
# find-structures-decoded-behind-calls.py <binary> [--symbols <file with the symbol table>] [--only <regex of function names>] [--offset <n>] [--expected <file>] [--show]
#
# The collector scans stacks and registers for pointers. It does not know a StructureID: 32 bits that become a Structure* when structureIDBase is added. A compiler is free to keep the
# ID across a call and to add the base behind it, even where the source decodes in front of the call and uses the pointer behind it, and ensureStillAliveHere() at the end does not forbid
# that. If nothing else holds the Structure, a collection inside that call frees it. WTF::opaque() around the decode makes the pointer a value that cannot be made anew.
#
# This reads the machine code of a finished binary, x86-64 or arm64, ELF or Mach-O, with llvm-nm and llvm-objdump, and lists every add of structureIDBase whose ID crossed a call: from the
# add it walks the function's own branches back, through copies and masks, to the instruction that gave the ID its value. Calls named in calls-that-never-collect.txt do not count.
# The offset of structureIDBase in g_config differs between platforms: without --offset it is the offset that is added to a register most often.
#
# A function that is listed in --expected (one name a line, '#' starts a comment that says why it is harmless) is not reported. Exit status 1 if anything else is found.
import re, subprocess, sys, collections
a = sys.argv[1:]; binary = a[0]; symbols = binary; only = re.compile(r'AOT|llint_slow_path'); offset = None; expected = set(); show = '--show' in a; names_only = '--names-only' in a
for i, x in enumerate(a):
    if x == '--symbols': symbols = a[i + 1]
    if x == '--only': only = re.compile(a[i + 1])
    if x == '--offset': offset = int(a[i + 1], 0)
    if x == '--expected': expected = {l.split('#')[0].strip() for l in open(a[i + 1]) if l.split('#')[0].strip()}
def run(c): return subprocess.run(c, capture_output=True, text=True, errors='replace').stdout
funcs = []; config = None
for l in run(['llvm-nm', '--defined-only', '-S', '-n', symbols]).split('\n'):
    p = l.split()
    if len(p) == 4 and p[2] in 'tTwW': funcs.append((int(p[0], 16), int(p[1], 16), p[3]))
    elif len(p) == 3 and p[1] in 'tT': funcs.append((int(p[0], 16), 0, p[2]))
    if p and p[-1] in ('g_config', '_g_config'): config = int(p[0], 16)
if config is None: print('no g_config in the symbol table'); sys.exit(2)
funcs.sort()
for i, (ad, sz, n) in enumerate(funcs):
    if not sz and i + 1 < len(funcs): funcs[i] = (ad, funcs[i + 1][0] - ad, n)
names = [f[2] for f in funcs]
out = subprocess.run(['llvm-cxxfilt', '-n'], input='\n'.join(names), capture_output=True, text=True).stdout.split('\n')
dem = dict(zip(names, out))
wanted = [f for f in funcs if only.search(dem.get(f[2], f[2])) or only.search(f[2])]
head = run(['llvm-objdump', '-f', binary])
arm = 'aarch64' in head or 'arm64' in head
X64 = {}
for r64, r32 in [('rax', 'eax'), ('rbx', 'ebx'), ('rcx', 'ecx'), ('rdx', 'edx'), ('rsi', 'esi'), ('rdi', 'edi'), ('rbp', 'ebp'), ('rsp', 'esp')] + [('r%d' % k, 'r%dd' % k) for k in range(8, 16)]: X64[r32] = r64; X64[r64] = r64
def canon(r):
    r = r.strip()
    if arm: return 'x' + r[1:] if re.match(r'^[wx]\d+$', r) else r
    return X64.get(r, r)
def disassemble_all(wanted):
    # one run of objdump over the span of the wanted functions; instructions are dealt out by address
    lo = min(f[0] for f in wanted); hi = max(f[0] + f[1] for f in wanted)
    c = ['llvm-objdump', '-d', '--no-show-raw-insn', '--start-address=0x%x' % lo, '--stop-address=0x%x' % hi, binary]
    if not arm: c.insert(1, '--x86-asm-syntax=intel')
    spans = sorted((f[0], f[0] + f[1], f[2]) for f in wanted); q = 0; got = collections.defaultdict(list)
    pr = subprocess.Popen(c, stdout=subprocess.PIPE, text=True, errors='replace')
    pattern = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$')
    for l in pr.stdout:
        m = pattern.match(l)
        if not m: continue
        pc = int(m.group(1), 16)
        while q < len(spans) and pc >= spans[q][1]: q += 1
        if q >= len(spans): break
        if pc >= spans[q][0]: got[spans[q][2]].append((pc, m.group(2), m.group(3)))
    pr.kill()
    return got
def operands(rest):
    t = rest.split('//')[0].split('<')[0]
    if not arm: t = t.split('#')[0]
    return [o.strip() for o in t.split(',')]
def follow_address_of_config(op, rest, ops, held):
    # x86-64: which registers hold an address inside g_config, and which address inside it this instruction reads, or None
    read = None
    for m in re.finditer(r'0x([0-9a-f]+)', rest):
        if op != 'lea' and config <= int(m.group(1), 16) < config + 0x4000: read = int(m.group(1), 16)
    m = re.search(r'\[(\w+)(?: \+ 0x([0-9a-f]+))?\]', rest.split('#')[0])
    if m and op != 'lea' and canon(m.group(1)) in held: read = held[canon(m.group(1))] + int(m.group(2) or '0', 16)
    if is_call(op):
        for r in list(held):
            if r not in ('rbx', 'rbp', 'r12', 'r13', 'r14', 'r15'): held.pop(r)
    elif ops and ops[0] and op not in ('cmp', 'test', 'push') and not op.startswith('j') and not ops[0].startswith(('[', 'qword', 'dword', 'word', 'byte')):
        held.pop(canon(ops[0]), None)
        named = [int(x, 16) for x in re.findall(r'0x([0-9a-f]+)', rest) if config <= int(x, 16) < config + 0x4000]
        if named and (op == 'lea' or (op in ('mov', 'movabs') and '[' not in rest.split('#')[0])): held[canon(ops[0])] = named[-1]
    return read
def is_call(op): return op in ('call', 'bl', 'blr', 'callq')
rows = []
wanted = [f for f in wanted if f[1] and f[1] <= 400000]
code = disassemble_all(wanted)
if offset is None:
    loads = collections.Counter()
    for ins in code.values():
        pages = {}
        for pc, op, rest in ins:
            if arm:
                ops = operands(rest)
                if op == 'adrp':
                    m = re.search(r'0x([0-9a-f]+)', rest)
                    if m: pages[canon(ops[0])] = int(m.group(1), 16)
                elif op == 'ldr' and ops[0].startswith('x'):
                    m = re.match(r'^\[(x\d+)(?:,#(0x[0-9a-f]+|\d+))?\]$', ','.join(ops[1:]).replace(' ', ''))
                    if m and m.group(1) in pages and config <= pages[m.group(1)] + int(m.group(2) or '0', 0) < config + 0x4000: loads[pages[m.group(1)] + int(m.group(2) or '0', 0) - config] += 1
            else:
                read = follow_address_of_config(op, rest, operands(rest), pages)
                if read is not None and op in ('add', 'mov'): loads[read - config] += 3 if op == 'add' else 1
    if not loads: print('nothing reads g_config'); sys.exit(2)
    offset = loads.most_common(1)[0][0]
target = config + offset
for ad, sz, n in wanted:
    ins = code.get(n, [])
    if not ins: continue
    # which registers hold the base, by address order
    decodes = []   # (index, id register)
    pages = {}; base = set()
    for k, (pc, op, rest) in enumerate(ins):
        ops = operands(rest)
        if arm:
            if op == 'adrp':
                m = re.search(r'0x([0-9a-f]+)', rest)
                if m: pages[canon(ops[0])] = int(m.group(1), 16)
                base.discard(canon(ops[0])); continue
            if op == 'ldr' and len(ops) >= 2:
                m = re.match(r'^\[(x\d+)(?:, #(0x[0-9a-f]+|\d+))?\]$', ','.join(ops[1:]).replace(' ', '').replace(',#', ', #'))
                d = canon(ops[0])
                if m and m.group(1) in pages and pages[m.group(1)] + int(m.group(2) or '0', 0) == target: base.add(d); pages.pop(d, None); continue
                base.discard(d); pages.pop(d, None); continue
            if op == 'add' and len(ops) >= 3:
                x, y = canon(ops[1]), canon(ops[2])
                if x in base and re.match(r'^x\d+$', y): decodes.append((k, y))
                elif y in base and re.match(r'^x\d+$', x): decodes.append((k, x))
                d = canon(ops[0]); base.discard(d); pages.pop(d, None); continue
            if is_call(op):
                for r in list(base):
                    if int(r[1:]) < 19: base.discard(r)
                for r in list(pages):
                    if int(r[1:]) < 19: pages.pop(r)
                continue
            if ops and re.match(r'^[wx]\d+$', ops[0]) and not op.startswith(('st', 'cmp', 'tst', 'cb', 'tb', 'b.')): base.discard(canon(ops[0])); pages.pop(canon(ops[0]), None)
        else:
            names_target = follow_address_of_config(op, rest, ops, pages) == target
            if names_target and op == 'add': decodes.append((k, canon(ops[0]))); base.discard(canon(ops[0])); continue
            if names_target and op == 'mov' and canon(ops[0]) in X64.values(): base.add(canon(ops[0])); continue
            if op == 'add' and len(ops) == 2 and canon(ops[1]) in base and canon(ops[0]) in X64.values(): decodes.append((k, canon(ops[0]))); continue
            if op == 'add' and len(ops) == 2 and canon(ops[0]) in base and canon(ops[1]) in X64.values(): decodes.append((k, canon(ops[1]))); base.discard(canon(ops[0])); continue
            if op == 'lea':
                m = re.match(r'^\[(\w+) \+ (\w+)\]$', ops[1]) if len(ops) > 1 else None
                if m and canon(m.group(1)) in base: decodes.append((k, canon(m.group(2))))
                elif m and canon(m.group(2)) in base: decodes.append((k, canon(m.group(1))))
                base.discard(canon(ops[0])); continue
            if is_call(op):
                for r in list(base):
                    if r not in ('rbx', 'rbp', 'r12', 'r13', 'r14', 'r15'): base.discard(r)
                continue
            if ops and canon(ops[0]) in base and op not in ('cmp', 'test', 'push') and not op.startswith('j'): base.discard(canon(ops[0]))
    # predecessors by the branches of the function itself
    index = {pc: q for q, (pc, _, _) in enumerate(ins)}
    preds = collections.defaultdict(list)
    for q, (pc, op, rest) in enumerate(ins):
        uncond = op in ('jmp', 'b', 'ret', 'br', 'ud2', 'brk', 'retq', 'int3', 'udf')
        if not uncond and q + 1 < len(ins): preds[q + 1].append(q)
        if (op.startswith('j') or op in ('b', 'cbz', 'cbnz', 'tbz', 'tbnz') or op.startswith('b.')) and not is_call(op):
            m = re.findall(r'0x([0-9a-f]+)', rest.split('<')[0])
            if m and int(m[-1], 16) in index: preds[index[int(m[-1], 16)]].append(q)
    def saved(r): return (int(r[1:]) >= 19) if arm and re.match(r'^x\d+$', r) else (not arm and r in ('rbx', 'rbp', 'r12', 'r13', 'r14', 'r15'))
    for k, reg in decodes:
        found = {}   # (where the ID got its value, why) -> calls crossed
        work = [(q, reg, ()) for q in preds[k]]; visited = set()
        while work and len(visited) < 60000:
            j, r, crossed = work.pop()
            if (j, r, bool(crossed)) in visited: continue
            visited.add((j, r, bool(crossed)))
            pc, op, rest = ins[j]; ops = operands(rest)
            if is_call(op):
                if not saved(r): continue   # the call's own result, or clobbered: no ID crosses here
                crossed = crossed + (j,) if len(crossed) < 8 else crossed
            else:
                d = canon(ops[0]) if ops and ops[0] else None
                writes = d == r and not op.startswith(('st', 'cmp', 'tst', 'test', 'cb', 'tb', 'b.', 'j', 'push')) and op not in ('b', 'ret')
                if arm and op == 'ldp' and len(ops) > 1 and canon(ops[1]) == r: writes = True
                if not arm and op == 'pop' and d == r: writes = True
                if writes:
                    src = canon(ops[-1]) if len(ops) >= 2 else None
                    isreg = src is not None and (src in X64.values() if not arm else bool(re.match(r'^x\d+$', src)))
                    if op in ('mov', 'orr') and isreg and len(ops) <= 3 and (not arm or len(ops) == 2 or canon(ops[1]) in ('xzr', 'wzr')): r = src
                    elif op in ('and', 'bic', 'ubfx', 'uxtw', 'lsr', 'shr', 'btr'):
                        if arm and len(ops) >= 2: r = canon(ops[1])
                    else:
                        why = 'from-a-stack-slot' if re.search(r'\[(sp|x29|rsp|rbp)\b', rest) and op in ('ldr', 'ldur', 'mov', 'ldp', 'ldrsw', 'pop') else 'defined'
                        if crossed or why == 'from-a-stack-slot': found.setdefault((j, why), crossed)
                        continue
            if not preds[j]:
                if crossed: found.setdefault((j, 'argument'), crossed)
                continue
            for q in preds[j]: work.append((q, r, crossed))
        for (j, why), crossed in found.items(): rows.append((n, ins, j, k, list(crossed), why, reg))
import bisect, os
starts = [f[0] for f in funcs]
def name_at(text):
    m = re.search(r'0x([0-9a-f]+)', text)
    if not m: return '(indirect)'
    q = bisect.bisect_right(starts, int(m.group(1), 16)) - 1
    return re.sub(r'\(.*', '', dem.get(funcs[q][2], funcs[q][2])).replace('JSC::', '') if q >= 0 and int(m.group(1), 16) < funcs[q][0] + max(funcs[q][1], 1) else '(outside)'
SAFE = re.compile('|'.join(l.strip() for l in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'calls-that-never-collect.txt')) if l.strip() and not l.startswith('#')))
kept = []
for row in rows:
    n, ins, j, k, calls, why, reg = row
    calls = [c for c in calls if not SAFE.search(name_at(ins[c][2]))]
    if calls or why == 'from-a-stack-slot': kept.append((n, ins, j, k, calls, why, reg))
rows = kept
seen = collections.OrderedDict()
for n, ins, j, k, calls, why, reg in rows: seen.setdefault(n, []).append((ins, j, k, calls, why, reg))
unexpected = [n for n in seen if re.sub(r'\(.*', '', dem.get(n, n)).lstrip('_') not in expected and n.lstrip('_') not in expected]
print('%s: %s, structureIDBase = g_config + 0x%x, %d functions read, %d decode a StructureID behind a call that it crossed, %d of them not expected' % (binary.split('/')[-1], 'arm64' if arm else 'x86-64', offset, len(wanted), len(seen), len(unexpected)))
for n in unexpected:
    l = seen[n]
    print('%s: %d' % (re.sub(r'\(.*', '', dem.get(n, n)), len(l)))
    if names_only: continue
    for ins, j, k, calls, why, reg in l:
        def callee(c): return name_at(ins[c][2])
        print('   decoded at +0x%x from %s (%s); the ID was %s at +0x%x; calls between: %s' % (ins[k][0] - ins[0][0], reg, ' '.join(ins[k][1:]).split('#')[0].strip(), {'defined': 'loaded', 'argument': 'an argument', 'from-a-stack-slot': 'reloaded from the stack'}[why], ins[max(j, 0)][0] - ins[0][0], ', '.join(callee(c) for c in reversed(calls[:6]))))
        if show:
            for q in sorted(set([max(j, 0)] + calls + [k])): print('      +0x%x %s %s' % (ins[q][0] - ins[0][0], ins[q][1], ins[q][2][:110]))
sys.exit(1 if unexpected else 0)
