#!/bin/bash
# Syntax-checks edited JavaScriptCore sources against the headers of a prebuilt WebKit (no full build).
# usage: syntax-check.sh <prebuilt dir, e.g. /root/.bun/build-cache/webkit-preview-pr-838-<sha8>-lto> <file relative to Source/JavaScriptCore>...
# The edited headers of the source tree win: /tmp/sc/JavaScriptCore links each private header to the source tree.
PRE=$1; shift; SRC=/workspace/WebKit/Source/JavaScriptCore
rm -rf /tmp/sc && mkdir -p /tmp/sc/JavaScriptCore
python3 - "$PRE" <<'PY'
import os, json, shlex, sys
pre = sys.argv[1]; src = '/workspace/WebKit/Source/JavaScriptCore'
idx = {}
for root, dirs, files in os.walk(src):
    for f in files:
        if f.endswith('.h'): idx.setdefault(f, []).append(os.path.join(root, f))
for f in os.listdir(pre + '/include/JavaScriptCore'):
    p = pre + '/include/JavaScriptCore/' + f
    c = idx.get(f, [])
    os.symlink(c[0] if (len(c) == 1 and not os.path.isdir(p)) else p, '/tmp/sc/JavaScriptCore/' + f)
e = [e for e in json.load(open(pre + '/compile_commands.json')) if 'UnifiedSource-heap-1.cpp' in e['file']][0]
out = []; skip = False
for a in shlex.split(e['command'])[1:]:
    if skip: skip = False; continue
    if a in ('-o', '-MF', '-MT', '-c', '-isystem'): skip = True; continue
    if a.startswith(('-I', '-MD', '-fdiagnostics-color', '-Wl,', '-flto', '-O3', '-g', '-fwhole', '-fforce', '-fno-split', '-mllvm', '-dwarf', '-ffile-prefix')) or a.endswith('.cpp') or a == '-fcolor-diagnostics': continue
    if a == '-Winvalid-pch': break
    out.append(a)
open('/tmp/sc/flagsline.txt', 'w').write(' '.join(out))
PY
INCS="-I/tmp/sc/JavaScriptCore -I/tmp/sc -I$PRE/include -I$SRC $(find $SRC -maxdepth 3 -type d | grep -v "/tests\|/Scripts\|/JavaScriptCore.xcodeproj\|/shell\|/testmem\|/API/tests" | sed 's/^/-I/' | tr '\n' ' ') -I$PRE/include/wtf -isystem $PRE/include/glibc"
for f in "$@"; do echo "=== $f"; /usr/lib/llvm-23/bin/clang++ $(cat /tmp/sc/flagsline.txt) -O1 -fsyntax-only $INCS $SRC/$f 2>&1 | grep -E "error|fatal|warning" | head -30; echo "rc=${PIPESTATUS[0]}"; done
