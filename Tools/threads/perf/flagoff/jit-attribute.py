#!/usr/bin/env python3
# jit-attribute.py <perf.data> <jitdump file> [-t] : attribute samples to C++ symbols or, for JIT addresses, to the code named in
# the jitdump (JIT_CODE_LOAD records). Prints counts per class (tier prefix of the JIT name) and, with -t, top names.
import sys, struct, subprocess, bisect, collections, re
data, dump = sys.argv[1], sys.argv[2]
b = open(dump, "rb").read()
magic, version, total, elf, pad, pid, ts, flags = struct.unpack_from("<IIIIIIQQ", b, 0)
off = total
loads = []  # (addr, size, name, order)
n = 0
while off + 16 <= len(b):
    rid, rsize, rts = struct.unpack_from("<IIQ", b, off)
    if rsize == 0: break
    if rid == 0:
        p, t, vma, addr, size, idx = struct.unpack_from("<IIQQQQ", b, off + 16)
        s = off + 16 + 40
        e = b.index(b"\0", s)
        loads.append((addr, size, b[s:e].decode("utf-8", "replace"), rts)); n += 1
    off += rsize
loads.sort()
starts = [l[0] for l in loads]
def lookup(a, t):
    # JIT memory is reused and the dump has no unload records: among the loads covering the address take the latest
    # one that happened before the sample (both clocks are CLOCK_MONOTONIC, nanoseconds).
    i = bisect.bisect_right(starts, a) - 1
    best = None; first = None
    while i >= 0 and starts[i] + (1 << 22) > a:
        l = loads[i]
        if l[0] <= a < l[0] + l[1]:
            if l[3] <= t + 2000000 and (best is None or l[3] > best[3]): best = l
            if first is None or l[3] < first[3]: first = l
        i -= 1
    return (best or first)[2] if (best or first) else None
out = subprocess.run(["perf", "script", "-i", data, "-F", "time,ip,sym"], capture_output=True, text=True).stdout
cls = collections.Counter(); names = collections.Counter(); cpp = collections.Counter(); tot = 0
def tier(name):
    m = re.match(r"^(Baseline|DFG|FTL|LLInt|Thunk|Yarr|RegExp|IC|InlineCache|Wasm|OSR|Handler|Stub|BBQ|OMG)\b", name)
    if m: return "JIT:" + m.group(1)
    return "JIT:" + name.split(":")[0].split(" ")[0][:16]
for l in out.split("\n"):
    p = l.split(None, 2)
    if len(p) < 2: continue
    try: t = int(float(p[0].rstrip(":")) * 1e9); a = int(p[1], 16)
    except ValueError: continue
    tot += 1
    sym = p[2].strip() if len(p) > 2 else "[unknown]"
    # An address inside a JIT load is JIT code whatever perf resolved it to: in a binary whose symbol table ends just below the
    # executable pool, perf names such addresses after the nearest preceding symbol (vmEntryToJavaScript).
    nm = lookup(a, t)
    if nm:
        cls[tier(nm)] += 1; names[nm] += 1
    elif sym == "[unknown]" or sym.startswith("0x"):
        cls["JIT:unnamed"] += 1
    else:
        cpp[re.sub(r"\(.*", "", sym)[:80]] += 1; cls["C++"] += 1
print("samples", tot, "loads", len(loads))
for k, v in cls.most_common(): print("%8d %5.1f%%  %s" % (v, 100.0 * v / tot, k))
if "-t" in sys.argv:
    for k, v in names.most_common(15): print("   %6d  %s" % (v, k[:110]))
if "-dump" in sys.argv:
    import json
    json.dump({"tot": tot, "cls": cls, "cpp": cpp, "names": names}, open(sys.argv[sys.argv.index("-dump") + 1], "w"))
