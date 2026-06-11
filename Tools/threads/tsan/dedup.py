#!/usr/bin/env python3
"""Dedup TSAN reports from per-test logs into stack-pair families."""
import sys, os, re, glob, collections

WRAP = re.compile(r'std::|__exchange|WTF::RawPtrTraits|WTF::RefPtr|WTF::Ref<|WTF::Detail|WTF::Function|WTF::Thread::entryPoint|wtfThreadEntryPoint|memcpy|memset|__interceptor|WTF::Vector<|WTF::HashTable|operator new|operator delete')

def frames(block):
    out = []
    for ln in block:
        m = re.match(r'\s+#(\d+) (\S+)', ln)
        if not m: break
        fn = ln.split(None, 2)[2] if len(ln.split(None,2))>2 else ''
        # fn like "JSC::Foo::bar(...) /path (jsc+0x..)" — take symbol up to '('
        sym = re.match(r'\s+#\d+ (.+?)(\(| /)', ln)
        out.append(sym.group(1) if sym else ln.strip())
    return out

def anchor(fr):
    for f in fr:
        if WRAP.search(f): continue
        return f
    return fr[0] if fr else '?'

def parse(path):
    txt = open(path, errors='replace').read()
    reports = []
    for m in re.finditer(r'WARNING: ThreadSanitizer: (.+?) \(pid=\d+\)\n(.*?)\n==================', txt, re.S):
        kind, body = m.group(1), m.group(2)
        lines = body.split('\n')
        stacks = []  # list of (desc, frames)
        i = 0
        while i < len(lines):
            mm = re.match(r'\s*((?:Atomic )?(?:Read|Write|Previous read|Previous write|Previous atomic read|Previous atomic write) of size \d+).*by (.+?):', lines[i])
            if mm:
                j = i+1; blk=[]
                while j < len(lines) and re.match(r'\s+#\d+ ', lines[j]):
                    blk.append(lines[j]); j+=1
                stacks.append((mm.group(1), frames(blk)))
                i = j
            else:
                i += 1
        reports.append((kind, stacks, m.group(0)))
    return reports

def main():
    logs = sorted(glob.glob(sys.argv[1] + '/*.log'))
    fam = collections.defaultdict(list)
    for lg in logs:
        for kind, stacks, raw in parse(lg):
            if kind != 'data race' or len(stacks) < 2:
                key = (kind,) + tuple(anchor(s[1]) for s in stacks[:2])
            else:
                key = tuple(sorted([anchor(stacks[0][1]), anchor(stacks[1][1])]))
            fam[key].append((os.path.basename(lg), raw))
    for key, items in sorted(fam.items(), key=lambda kv: -len(kv[1])):
        tests = collections.Counter(t for t,_ in items)
        print('=' * 100)
        print(f'FAMILY count={len(items)} key={key}')
        print('  tests:', ', '.join(f'{t}x{c}' for t,c in tests.most_common(6)))
    print(f'TOTAL families={len(fam)} reports={sum(len(v) for v in fam.values())}')

if __name__ == '__main__':
    main()
