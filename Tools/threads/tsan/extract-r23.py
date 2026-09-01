#!/usr/bin/env python3
"""Extract one representative full report per deduped family from r23."""
import sys, re, glob, os
sys.path.insert(0, os.path.dirname(__file__))
from dedup import parse, anchor

seen = set()
logs = sorted(glob.glob('/root/WebKit/Tools/threads/tsan/r23/*.log'))
for lg in logs:
    for kind, stacks, raw in parse(lg):
        if kind != 'data race' or len(stacks) < 2:
            key = (kind,) + tuple(anchor(s[1]) for s in stacks[:2])
        else:
            key = tuple(sorted([anchor(stacks[0][1]), anchor(stacks[1][1])]))
        if key in seen:
            continue
        seen.add(key)
        print('=' * 100)
        print(f'KEY: {key}')
        print(f'FROM: {os.path.basename(lg)}')
        # print trimmed: just the two stack blocks + location
        lines = raw.split('\n')
        out = []
        depth = 0
        for ln in lines:
            if re.match(r'\s+#\d+ ', ln):
                depth += 1
                if depth <= 8:
                    out.append(ln)
            else:
                if depth > 8:
                    out.append(f'    ... ({depth-8} more frames)')
                depth = 0
                if 'Thread T' in ln and 'created by' in ln:
                    break
                out.append(ln)
        print('\n'.join(out))
print(f'\nTOTAL unique keys: {len(seen)}')
