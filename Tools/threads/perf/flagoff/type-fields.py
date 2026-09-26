#!/usr/bin/env python3
"""type-fields.py <jsc with debug information> <type>[;<type>...]: size of each type and offset, size and name of every
non-static member, base classes flattened (through gdb's Python interface; its `ptype` does not find every name).
With two binaries, `type-fields.py <main jsc> <branch jsc> <types>` prints both side by side where they differ.
type-sizes.py finds the types whose size differs; this says which member did it."""
import os, subprocess, sys, tempfile
SCRIPT = r'''
import gdb, os
def show(t, base, out):
    for f in t.fields():
        if not hasattr(f, 'bitpos'): continue
        try: off = base + f.bitpos // 8
        except Exception: continue
        if f.is_base_class:
            show(f.type.strip_typedefs(), off, out); continue
        bits = (" :%d@%d" % (f.bitsize, f.bitpos % 8)) if f.bitsize else ""
        out.write("%5d %5d%s  %s  (%s)\n" % (off, f.type.sizeof, bits, f.name, str(f.type)[:70]))
out = open(os.environ['TYPE_FIELDS_OUT'], 'w')
for name in os.environ['TYPE_FIELDS_TYPES'].split(';'):
    try:
        t = gdb.lookup_type(name).strip_typedefs()
        out.write("== %s sizeof %d\n" % (name, t.sizeof))
        show(t, 0, out)
    except Exception as e:
        out.write("== %s: %s\n" % (name, e))
out.close()
'''
def fields(binary, types):
    with tempfile.TemporaryDirectory() as d:
        s = os.path.join(d, "s.py"); o = os.path.join(d, "o.txt")
        open(s, "w").write(SCRIPT)
        env = dict(os.environ, TYPE_FIELDS_OUT=o, TYPE_FIELDS_TYPES=types)
        subprocess.run(["gdb", "-q", "-batch", "-x", s, binary], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return open(o).read().split("\n") if os.path.exists(o) else ["gdb produced nothing for " + binary]
a = sys.argv[1:]
if len(a) == 2:
    print("\n".join(fields(a[0], a[1])))
elif len(a) == 3:
    import difflib
    x = fields(a[0], a[2]); y = fields(a[1], a[2])
    for l in difflib.unified_diff(x, y, "main", "branch", n=100, lineterm=""): print(l)
else:
    print(__doc__); sys.exit(2)
