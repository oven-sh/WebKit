#!/usr/bin/env python3
"""lint-upstream-removed-lines.py [<base ref, default origin/main>] [<since, default 8 months ago>]

Lines the branch adds to a file that the base's own history removed from that same file, and that the base no longer has:
what a rebase that replays the branch's old text leaves behind. Run it after every rebase and read each hit: most are the
branch's own code that happens to repeat a line upstream once had, a few are work upstream deleted and the branch still
does (thirteenth round: the metadata table was zero-filled twice, once by the allocator as upstream now does and once by
the memset upstream removed when it switched)."""
import collections, re, subprocess, sys
base = sys.argv[1] if len(sys.argv) > 1 else 'origin/main'
since = sys.argv[2] if len(sys.argv) > 2 else '8 months ago'
PATHS = ['Source/JavaScriptCore', 'Source/WTF']
def run(args):
    return subprocess.run(args, capture_output=True, text=True, errors='replace').stdout
removed = collections.defaultdict(dict)
cur = commit = None
for l in run(['git', 'log', '-p', '--no-color', '--since=' + since, '--format=commit %h %ad %s', '--date=short', base, '--'] + PATHS).split('\n'):
    if l.startswith('commit '): commit = l[7:]
    elif l.startswith('--- a/'): cur = l[6:]
    elif l.startswith('-') and not l.startswith('---') and cur:
        t = l[1:].strip()
        if len(t) > 28: removed[cur].setdefault(t, commit)
hits = collections.defaultdict(list); cache = {}; cur = None; lineno = 0
for l in run(['git', 'diff', '--no-color', '-U0', base, '--'] + PATHS).split('\n'):
    if l.startswith('+++ b/'): cur = l[6:]
    elif l.startswith('@@'):
        lineno = int(re.search(r'\+(\d+)', l).group(1)) - 1
    elif l.startswith('+') and not l.startswith('+++') and cur:
        lineno += 1
        t = l[1:].strip()
        if t in removed.get(cur, {}) and not re.search(r'^(//|\*|#include|return|break;|else|\{|\})', t):
            if cur not in cache:
                cache[cur] = set(x.strip() for x in run(['git', 'show', base + ':' + cur]).split('\n'))
            if t not in cache[cur]:
                hits[cur].append((lineno, t, removed[cur][t]))
for f, hs in sorted(hits.items()):
    print(f)
    for ln, t, c in hs:
        print('   %5d  %s    [removed by %s]' % (ln, t[:110], c[:70]))
print('%d lines in %d files' % (sum(len(h) for h in hits.values()), len(hits)))
