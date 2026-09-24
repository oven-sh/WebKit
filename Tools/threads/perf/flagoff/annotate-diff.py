#!/usr/bin/env python3
"""annotate-diff.py <main jsc> <branch jsc> <symbol substring> [...] : the instructions of the same function in both binaries, with the
numbers that layout changes (displacements, immediates, branch targets, call targets) and the register names blanked, and the difference between the two
listings. Use it on the symbols profile-diff.py names: what the flag-off function executes that main's does not. Prints per symbol
the instruction counts and the added / removed instructions (a `diff` of the normalized listings)."""
import difflib, re, subprocess, sys
main, branch = sys.argv[1], sys.argv[2]

def symbols(binary, pat):
    out = subprocess.run(["nm", "-S", "-C", "--defined-only", binary], capture_output=True, text=True).stdout
    res = []
    for l in out.split("\n"):
        p = l.split(None, 3)
        if len(p) == 4 and pat in p[3] and " lambda" not in p[3]:
            res.append((p[3], int(p[0], 16), int(p[1], 16)))
    return res

def listing(binary, addr, size):
    out = subprocess.run(["objdump", "-d", "--no-show-raw-insn", "-C", "--start-address=%d" % addr, "--stop-address=%d" % (addr + size), binary],
                         capture_output=True, text=True).stdout
    lines = []
    for l in out.split("\n"):
        m = re.match(r"^\s*[0-9a-f]+:\s+(.*)$", l)
        if not m: continue
        t = m.group(1)
        t = re.sub(r"<[^>]*>", "<T>", t)          # branch and call targets
        t = re.sub(r"#\s*[0-9a-f]+.*$", "", t)     # rip-relative comment
        t = re.sub(r"-?0x[0-9a-f]+", "#", t)       # displacements, immediates, targets
        t = re.sub(r"\b[0-9a-f]{5,}\b", "#", t)     # bare addresses
        t = re.sub(r"%(r[0-9a-z]+|e[a-ds][xip]|[a-d][lx]|[sd]il?|[sb]pl?|xmm[0-9]+|ymm[0-9]+)\b", "%R", t)  # register allocation
        lines.append(re.sub(r"\s+", " ", t).strip())
    return lines

QUIET = "-q" in sys.argv
def kinds(l):
    c = {"insn": len(l)}
    c["byte-tests"] = sum(1 for x in l if re.match(r"(cmpb|testb) \$#,", x))
    c["locked"] = sum(1 for x in l if x.startswith("lock ") or x.startswith("xchg "))
    c["calls"] = sum(1 for x in l if x.startswith("call "))
    return c

for pat in [a for a in sys.argv[3:] if a != "-q"]:
    a = symbols(main, pat); b = symbols(branch, pat)
    if not a or not b:
        print("%s: not found (main %d, branch %d)" % (pat, len(a), len(b))); continue
    # pair by name; when the name occurs several times (instantiations) take the largest of each
    a.sort(key=lambda x: -x[2]); b.sort(key=lambda x: -x[2])
    for (na, aa, sa), (nb, ab, sb) in zip(a[:1], b[:1]):
        la = listing(main, aa, sa); lb = listing(branch, ab, sb)
        ka, kb = kinds(la), kinds(lb)
        print("== %s: main %d instructions (%d bytes), branch %d instructions (%d bytes); byte tests %d -> %d, locked %d -> %d, calls %d -> %d" % (
            na[:90], len(la), sa, len(lb), sb, ka["byte-tests"], kb["byte-tests"], ka["locked"], kb["locked"], ka["calls"], kb["calls"]))
        if QUIET: continue
        for d in difflib.unified_diff(la, lb, lineterm="", n=1):
            if d.startswith(("---", "+++")): continue
            print("   " + d)
