#!/usr/bin/env python3
# usage: tsan-dedupe.py <mirror outdir>: group TSAN reports by (access frame, previous frame) signature.
import sys, os, re, collections
d = sys.argv[1]
sig = collections.defaultdict(list)
frame_re = re.compile(r'^\s+#\d+ (\S+.*?) /.*?([A-Za-z0-9_]+\.(?:h|cpp)):(\d+)')
for f in sorted(os.listdir(d)):
    if not f.endswith('.out'): continue
    lines = open(os.path.join(d,f), errors='replace').read().splitlines()
    i = 0
    while i < len(lines):
        if lines[i].startswith('WARNING: ThreadSanitizer:'):
            kind = lines[i]
            # first frame after the "Read/Write of size" line
            a = b = None
            j = i+1
            while j < len(lines) and not lines[j].strip().startswith('#0'): j += 1
            if j < len(lines):
                m = frame_re.match(lines[j]); a = (m.group(1).split('(')[0][:70]+' @'+m.group(2)+':'+m.group(3)) if m else lines[j].strip()[:90]
            k = j+1
            while k < len(lines) and not lines[k].strip().startswith('Previous'): k += 1
            while k < len(lines) and not lines[k].strip().startswith('#0'): k += 1
            if k < len(lines):
                m = frame_re.match(lines[k]); b = (m.group(1).split('(')[0][:70]+' @'+m.group(2)+':'+m.group(3)) if m else lines[k].strip()[:90]
            sig[(re.sub(r" \(pid=\d+\)","",kind.replace("WARNING: ThreadSanitizer: ","")), a, b)].append(f[:-4])
            i = k
        i += 1
items = sorted(sig.items(), key=lambda kv: -len(kv[1]))
print(f"{len(items)} distinct signatures")
for (kind,a,b),files in items:
    print(f"[{len(files):3d}] {kind}\n      {a}\n   vs {b}\n      e.g. {files[0]}")
