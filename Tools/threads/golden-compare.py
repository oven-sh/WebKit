#!/usr/bin/env python3
"""golden-compare.py <main jsc> <branch jsc> [--runs N] [--tests FILE...] : the JIT code the branch generates with the threads flag off,
against the code `main` generates, for the same programs. Both binaries must be built with the disassembler (DevRelease:
-DBUN_ENABLE_JIT_DISASSEMBLER=1). Each program runs with the concurrent JIT off (so compilation order is deterministic) under three
option sets (Baseline only, DFG-eager, FTL-eager) and --dumpDisassembly; the output is cut into code blocks (the header line names the code
block by its bytecode hash), addresses and every hexadecimal number (displacements, immediates, jump and call targets) are blanked, and
matching blocks are compared instruction by instruction. `main` is also compared with itself (a second run), which is the noise floor:
blocks that differ between two runs of one binary.
Prints, per option set: blocks compared, identical, differing; the differing blocks' instruction-count deltas; and the blocks that differ
in a way `main` against `main` does not."""
import collections, difflib, os, re, subprocess, sys, tempfile

OPTS = {
    "baseline": ["--useConcurrentJIT=0", "--useDFGJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForJITSoon=10"],
    "dfg-eager": ["--useConcurrentJIT=0", "--useFTLJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForJITSoon=10", "--thresholdForOptimizeAfterWarmUp=100", "--thresholdForOptimizeAfterLongWarmUp=100", "--thresholdForOptimizeSoon=100"],
    "ftl-eager": ["--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForJITSoon=10", "--thresholdForOptimizeAfterWarmUp=100", "--thresholdForOptimizeAfterLongWarmUp=100", "--thresholdForOptimizeSoon=100", "--thresholdForFTLOptimizeAfterWarmUp=1000", "--thresholdForFTLOptimizeSoon=1000"],
}
HDR = re.compile(r"^(\S.*?)(?::| \[0x[0-9a-f]+, 0x[0-9a-f]+\))")
INSN = re.compile(r"^\s+0x[0-9a-f]+: (.*)$")

def run(jsc, opts, test):
    try:
        # A fixed seed: the JIT blinds constants with random numbers (an `xor` and a `ror` around an immediate), which would make every run different.
        p = subprocess.run([jsc, "--dumpDisassembly=1", "--dumpDFGDisassembly=1", "--dumpFTLDisassembly=1", "--seedOfVMRandomForFuzzer=1", "--forceWeakRandomSeed=1", "--forcedWeakRandomSeed=1"] + opts + [test], capture_output=True, text=True, timeout=120, errors="replace")
    except subprocess.TimeoutExpired:
        return None
    return p.stdout + p.stderr

def blocks(text):
    out = collections.OrderedDict(); cur = None; seen = collections.Counter()
    for l in text.split("\n"):
        m = INSN.match(l)
        if m and cur is not None:
            t = re.sub(r"-?0x[0-9a-f]+", "#", m.group(1)); out[cur].append(re.sub(r"\s+", " ", t).strip())
        elif l and not l[0].isspace() and ("Generated" in l or "JIT code for" in l or "code for" in l):
            h = re.sub(r"\[0x[0-9a-f]+, 0x[0-9a-f]+\)", "", l)
            h = re.sub(r"\d+ bytes", "", h); h = re.sub(r"0x[0-9a-f]+", "#", h).strip(": ")
            seen[h] += 1; cur = "%s |%d" % (h, seen[h]); out[cur] = []
        elif l and not l[0].isspace():
            cur = None
    pad = lambda i: i == "int3" or i.startswith("nop") or i.startswith("data16") or i.startswith("xchg %ax, %ax")
    for k in out:  # alignment padding at either end of a block, and inside it, is not code
        while out[k] and pad(out[k][-1]): out[k].pop()
        while out[k] and pad(out[k][0]): out[k].pop(0)
        out[k] = [i for i in out[k] if not pad(i)]
    return out

CODEBLOCK = re.compile(r"^Generated (Baseline|DFG|FTL) JIT code for [^|]*#[A-Za-z0-9]{6}:\[")

def compare(a, b):
    # Only the code of code blocks (named by their bytecode hash): thunks and inline-cache stubs are generated lazily and their
    # order changes from run to run, which shifts every later occurrence index.
    same = diff = 0; deltas = []
    for k in a:
        if k not in b or not CODEBLOCK.match(k): continue
        if a[k] == b[k]: same += 1
        else:
            diff += 1; deltas.append((len(b[k]) - len(a[k]), k, a[k], b[k]))
    return same, diff, deltas, [k for k in a if k not in b], [k for k in b if k not in a]

def one(job):
    main_jsc, branch_jsc, t = job
    res = []
    for name, opts in OPTS.items():
        a = run(main_jsc, opts, t); b = run(branch_jsc, opts, t); a2 = run(main_jsc, opts, t)
        if a is None or b is None or a2 is None: continue
        A, B, A2 = blocks(a), blocks(b), blocks(a2)
        s, d, deltas, om, ob = compare(A, B); s2, d2, deltas2, _, _ = compare(A, A2)
        noise = {k for _, k, _, _ in deltas2}
        real = [(dl, k, x, y) for dl, k, x, y in deltas if k not in noise]
        res.append((name, s + d, s, len(real), s2, d2, len(om), len(ob), os.path.basename(t), real))
    return res

def main():
    import multiprocessing, json
    args = sys.argv[1:]
    main_jsc, branch_jsc = args[0], args[1]; tests = [a for a in args[2:] if not a.startswith("--")]
    out = None
    for i, a in enumerate(args):
        if a == "--dump": out = args[i + 1]; tests = [t for t in tests if t != out]
    with multiprocessing.Pool(int(os.environ.get("GOLDEN_JOBS", "24"))) as pool:
        results = pool.map(one, [(main_jsc, branch_jsc, t) for t in tests], chunksize=1)
    tot = collections.defaultdict(lambda: [0, 0, 0, 0, 0, 0, 0]); examples = collections.defaultdict(list)
    for res in results:
        for name, n, same, real, s2, d2, om, ob, tname, blocks_ in res:
            r = tot[name]; r[0] += n; r[1] += same; r[2] += real; r[3] += s2; r[4] += d2; r[5] += om; r[6] += ob
            examples[name] += [(tname,) + x for x in blocks_]
    for name, r in tot.items():
        print("%-10s blocks %5d: identical %5d, differing (not noise) %4d, main-vs-main differing %3d, only in main %3d, only in branch %3d" % (name, r[0], r[1], r[2], r[4], r[5], r[6]))
        for t, delta, k, x, y in sorted(examples[name], key=lambda e: -abs(e[1]))[:6]:
            print("     %-28s %+4d instructions  %s" % (t, delta, k[:80]))
    if out:
        json.dump({name: [(t, dl, k, x, y) for t, dl, k, x, y in ex] for name, ex in examples.items()}, open(out, "w"))
main()
