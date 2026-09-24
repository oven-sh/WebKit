#!/usr/bin/env python3
"""type-sizes.py <main jsc> <branch jsc> [name-regex] : sizeof of the classes both binaries have debug information for, and the ones whose size
differs. One llvm-dwarfdump pass over each binary (about ten seconds). Restricts to JSC:: / WTF:: classes and structs whose names match the
regex (default: all). Objects a process allocates by the thousand (Structure, the executables, CodeBlock, JSFunction, PropertyTable, ...) are
where a few bytes of growth become memory: look at the rows that differ and multiply by how many the workload makes."""
import re, subprocess, sys
def sizes(binary, rx):
    p = subprocess.Popen(["llvm-dwarfdump-21", "--regex", "--name=" + rx, binary], stdout=subprocess.PIPE, text=True, errors="replace", bufsize=1 << 20)
    res = {}; cur = None
    for l in p.stdout:
        if "DW_TAG_" in l:
            cur = {"tag": l.split("DW_TAG_")[1].strip()}
        elif cur is not None:
            m = re.search(r'DW_AT_(\w+)\s+\((.*)\)', l)
            if not m: continue
            k, v = m.groups()
            if k == "name": cur["name"] = v.strip('"'); 
            elif k == "byte_size": cur["size"] = int(v, 0)
            elif k == "declaration": cur["decl"] = True
            if "name" in cur and "size" in cur and not cur.get("decl") and cur["tag"] in ("class_type", "structure_type"):
                res.setdefault(cur["name"], cur["size"]); 
    return res
rx = sys.argv[3] if len(sys.argv) > 3 else "^[A-Z]"
a = sizes(sys.argv[1], rx); b = sizes(sys.argv[2], rx)
common = sorted(set(a) & set(b))
diff = [(n, a[n], b[n]) for n in common if a[n] != b[n]]
print("classes with sizes in both: %d; differing: %d" % (len(common), len(diff)))
for n, x, y in sorted(diff, key=lambda t: -(t[2] - t[1])): print("%+5d  %5d -> %5d  %s" % (y - x, x, y, n))
