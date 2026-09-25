#!/usr/bin/env python3
"""flagoff-ledger.py: everything the branch makes a flag-off process execute that the base does not.

    flagoff-ledger.py --base <rev> [--head <rev>] [--ledger docs/threads/FLAG-OFF-LEDGER.tsv] [--summary] [--check | --update]

The branch has no build flag that compiles the threads work out: every difference between a process that never sets
`useJSThreads` and the base is a run-time test or an unconditional change. This tool lists the unconditional ones.

Method. It splits `git diff <base> <head> -- Source/{JavaScriptCore,WTF,bmalloc}` into added lines, takes the head version of
every changed C/C++ file with comments and string literals blanked, tracks brace scopes, and gives every added line one class:

    C   comment or blank, or a line that only makes the function compiled once per threads mode (JSC_PER_THREADS_MODE_BEGIN/END)
    N   in a file that is flag-on only by construction (NEW_FLAG_ON_FILES)
    M   whitespace-identical to a line the same file removes (moved or re-indented original code)
    D   outside any function body (declaration, member, include, macro)
    G+  inside a block whose header tests a flag-on gate, or after an early return on the negated gate: flag-on only
    G-  inside the negated arm (or the `else`) of a gate: the flag-off arm
    U   everything else: code a flag-off process can execute

Every U line (and every G- line) is attributed to its enclosing function, which is either NEW (its header is itself added by the
branch) or EXISTING. The ledger has one row per EXISTING function with at least one U line: the rows of code that runs in a flag-off
process and did not exist on the base. A row carries the class the tool guessed from its lines (`auto`), and the human-maintained
columns `verdict`, `reason` and `reviewer`, which survive regeneration (rows are keyed by file and function). `--check` fails when a
function has U lines and no row, or a row has no verdict; that is how a later change that adds an ungated hunk is made to say why.

The classifier over-counts by a few percent (a flag-on arm the scope tracker lost) and does not see gates it does not know: the
vocabulary is GATE below. Its precision was checked by hand on random samples (FLAG-OFF-LANDING 2.4.0). Two files defeat the
scope tracker (heap/Heap.cpp, ftl/FTLLowerDFGToB3.cpp): their line classes are right, their function attribution is not.
"""
import argparse, collections, json, os, re, subprocess, sys

GATE = (r"(useJSThreads|useTaggedButterflies|gilOffProcess|gilOff\(\)|gilOffWithProcessGate|m_gilOff\b|isGILOffProcess|useVMLite|"
        r"useSharedAtomStringTable|useSharedGCHeap|isSharedServer|isSharedHeapServer|\bjsThreads\b|anyJSThreadEverSpawned|isSpawned|"
        r"forceSegmentedButterflies|forceButterflySWBit|verifyConcurrentButterfly|validateButterflyTagDiscipline|useStructureAllocationLock|"
        r"useThreadGILOffUnsafe|useConcurrentSharedGCMarking|useSharedGC\w+|isSharedAtomStringTable|sharedAtomStringTableEnabled|"
        r"useAtomicDeferrableRefCount|RaceAmplifier::isEnabled|randomYieldPeriod|countJSThreadsCounters|reportJSThreadsCounters|"
        r"jsThreadsCountersEnabled|TaggedButterflies|GILOff\w*\(|isGILOff|\bgilOff\b|\bthreaded\b|useThreaded\w+|hasSpawnedThreads|"
        r"numberOfSpawnedThreads|liveSpawnedThread|ThreadsMode\w*|threadsMode\w*|processUses\w+|processIsGILOff|JSC_THREADS_MODE_IS_THREADED)")
# The lines that make a function compiled once per threads mode (runtime/ThreadsModePage.h): they select a copy, they are not code of either.
PER_MODE_SCAFFOLD = re.compile(r"^\s*(JSC_PER_THREADS_MODE_BEGIN\(.*\)|JSC_PER_THREADS_MODE_END(_WITHOUT_RETURN)?|JSC_THREADS_MODE_BODY\(\w+\);)\s*$")
NEW_FLAG_ON_FILES = set("""bytecode/JSThreadsSafepoint.cpp bytecode/RetiredJITArtifacts.cpp dfg/DFGPollVisibilityPhase.cpp
heap/GCThreadLocalCache.cpp heap/HeapClientSet.cpp heap/SharedHeapTestHarness.cpp jit/ConcurrentButterflyOperations.cpp
runtime/ConcurrentButterfly.cpp runtime/ConditionObject.cpp runtime/LockObject.cpp runtime/ThreadAtomics.cpp
runtime/ThreadLocalObject.cpp runtime/ThreadManager.cpp runtime/ThreadObject.cpp runtime/RaceAmplifier.cpp""".split())
FLAGON_NAME = re.compile(r"(Concurrent|GILOff|GilOff|Threaded|Segmented|Tagged|Spawned|VMLite|Lite\b|SharedGC|SharedServer|SharedHeap|"
                         r"sharedHeap|JSThreads|jsThreads|ThreadLocal|ForThreads|RaceAmplifier|SharedAtom|perLite|PerLite|Carrier|carrier|"
                         r"Conductor|conductor|StopTheWorldFor|HeapClient|GCClient|ButterflyTID|butterflyTID|TID\b|spine|Spine|fragment|"
                         r"Fragment|quarantine|Quarantine|deferredClaim|DeferredClaim|claim[A-Z]|Claim|parkSite|ParkSite|safepoint|Safepoint)")
# What counts as "an inserted gate test" in the summary: a gate, or a mention of the per-thread structures, the mode-split
# accessors, a diagnostic counter or the race amplifier (FLAG-OFF-LANDING 2.4.1).
GATE_MENTION = GATE[:-1] + (r"|group3Primitives|trapsForCurrentThread|currentThreadEntryScope|JSThreadsCounters|RaceAmplifier|VMLite|"
                            r"threadsLocker|gilOffLocker|perThread|PerThread|currentLite|VMThread|ownerThread)")
SOURCE_DIRS = ["Source/JavaScriptCore", "Source/WTF", "Source/bmalloc"]
C_LIKE = (".cpp", ".h", ".mm", ".c")


def git(repo, *a):
    return subprocess.run(("git",) + a, cwd=repo, capture_output=True, text=True, errors="replace").stdout


def strip_comments(text):
    """Lines with // and /* */ comments and string/char literals blanked."""
    out = []; inblock = False
    for line in text.split("\n"):
        res = []; i = 0; n = len(line); instr = None
        while i < n:
            c = line[i]
            if inblock:
                if line.startswith("*/", i): inblock = False; i += 2
                else: i += 1
                continue
            if instr:
                if c == "\\": i += 2; continue
                if c == instr: instr = None
                res.append(" "); i += 1; continue
            if line.startswith("//", i): break
            if line.startswith("/*", i): inblock = True; i += 2; continue
            if c in "\"'": instr = c; res.append(" "); i += 1; continue
            res.append(c); i += 1
        out.append("".join(res))
    return out


def scopes(lines):
    """For each line, the stack of [header, else-of] of the braces open at its start."""
    stack = []; per = []; header = ""; last_closed = None; in_pp = False
    for ln in lines:
        per.append([list(x) for x in stack])
        if in_pp or ln.lstrip().startswith("#"):
            in_pp = ln.rstrip().endswith("\\")   # a directive continues on the next line
            continue
        for ch in ln:
            if ch == "{":
                h = header.strip()
                stack.append([h, last_closed if re.match(r"^else\b", h) else None]); header = ""; last_closed = None
            elif ch == "}":
                if stack: last_closed = stack.pop()
                header = ""
            elif ch == ";":
                header = ""; last_closed = None
            else:
                header += ch
        header += " "
    return per


class Classifier:
    def __init__(self):
        self.gate = GATE
        self.aliases = None

    def gate_re(self):
        return self.gate if not self.aliases else self.gate[:-1] + "|" + self.aliases + ")"

    def polarity(self, h):
        """+1 positive gate, -1 negative, 0 none or mixed; +2 a function whose name says GILOff."""
        G = self.gate_re()
        if not re.search(G, h): return 0
        if not re.match(r"^(else\s+)?(if|while|for)\b|^(else\s+)?if\s+constexpr", h) and "?" not in h:
            if re.search(r"GILOff\w*\s*\(", h) and "(" in h and not h.startswith(("if", "else", "while", "for", "switch", "return")): return +2
            return 0
        if "||" in h: return 0
        pos = neg = 0
        for m in re.finditer(G, h):
            pre = h[:m.start()]
            j = len(pre.rstrip()); k = j
            while k > 0 and re.match(r"[\w\.\>\-\:\(\)\*\&\s]", pre[k - 1]) and pre[k - 1] != "!":
                if pre[k - 1] in "&|,": break
                k -= 1
            if k > 0 and pre[k - 1] == "!": neg += 1
            else: pos += 1
        if pos and not neg: return +1
        if neg and not pos: return -1
        if pos and neg and re.search(r"useJSThreads|useTaggedButterflies|gilOff|jsThreads|isSharedServer", h): return +1
        return 0

    def classify_file(self, rel, text, added_lines, removed_norm):
        raw = text.split("\n"); code = strip_comments(text)
        al = set()
        for ln in code:
            m = re.match(r"^\s*(?:static\s+|const\s+|constexpr\s+)*(?:bool|auto)\s+(\w+)\s*=\s*(.*)$", ln)
            if m and re.search(GATE, m.group(2)) and "||" not in m.group(2):
                g = re.search(GATE, m.group(2)); pre = m.group(2)[:g.start()].rstrip()
                if not pre.endswith("!") and len(m.group(1)) > 3: al.add(m.group(1))
        self.aliases = "|".join(r"\b" + re.escape(a) + r"\b" for a in sorted(al)) if al else None
        per = scopes(code) if rel.endswith(C_LIKE) else None
        early = {}
        if per:
            ids = []; sid = 0; st = []
            for ln in code:
                ids.append(tuple(st))
                for ch in ln:
                    if ch == "{": sid += 1; st.append(sid)
                    elif ch == "}" and st: st.pop()
            gated = set()
            for idx, ln in enumerate(code):
                cur = ids[idx]
                early[idx] = any(s in gated for s in cur)
                m = re.search(r"\bif\s*\((.*)\)\s*(\[\[\w+\]\]\s*)?(return|continue|break)\b", ln)
                if m and cur:
                    if self.polarity("if (" + m.group(1) + ")") == -1: gated.add(cur[-1])
                elif idx + 1 < len(code) and re.search(r"\bif\s*\((.*)\)\s*(\[\[\w+\]\]\s*)?$", ln) and re.match(r"\s*(return|continue|break)\b", code[idx + 1]) and cur:
                    if self.polarity("if (" + re.search(r"\bif\s*\((.*)\)", ln).group(1) + ")") == -1: gated.add(cur[-1])
        res = {}
        short = rel.split("Source/JavaScriptCore/")[-1]
        for ln in added_lines:
            idx = ln - 1
            if idx >= len(code): res[ln] = "U"; continue
            if not code[idx].strip() or PER_MODE_SCAFFOLD.match(raw[idx]): res[ln] = "C"; continue
            if short in NEW_FLAG_ON_FILES: res[ln] = "N"; continue
            norm = re.sub(r"\s+", "", raw[idx])
            if norm in removed_norm and len(norm) > 3: res[ln] = "M"; continue
            if per is None: res[ln] = "U"; continue
            stk = per[idx]
            if not stk: res[ln] = "D"; continue
            pol = 0; infunc = False
            for h, elseof in stk:
                p = self.polarity(h)
                if p == 0 and elseof is not None and re.match(r"^else\b", h) and not re.search(GATE, h):
                    pe = self.polarity(elseof[0])
                    if pe == +1: p = -1
                    elif pe == -1: p = +1
                if p in (+1, +2): pol = +1
                elif p == -1 and pol == 0: pol = -1
                if "(" in h and not re.match(r"^(class|struct|namespace|enum|union)\b", h): infunc = True
            if pol == 0:
                c = code[idx].strip()
                m = re.match(r"^(else\s+)?if\s*\((.*)\)\s*[^{;]+;\s*$", c)
                if m:
                    p = self.polarity("if (" + m.group(2) + ")")
                    if p: pol = p
                elif idx > 0 and re.match(r"^(else\s+)?if\s*\(.*\)\s*(\[\[\w+\]\])?\s*$", code[idx - 1].strip()):
                    p = self.polarity(code[idx - 1].strip())
                    if p: pol = p
            if pol == 0 and early.get(idx): pol = +1
            if pol == +1: res[ln] = "G+"
            elif pol == -1: res[ln] = "G-"
            elif not infunc: res[ln] = "D"
            else: res[ln] = "U"
        return res, code


def function_spans(code):
    """(header start line, open line, close line, header text) of outermost function bodies."""
    spans = []; st = []; header = ""; hstart = None
    notfunc = r"^(class|struct|namespace|enum|union|extern)\b"
    in_pp = False
    for i, ln in enumerate(code):
        if in_pp or ln.lstrip().startswith("#"):
            in_pp = ln.rstrip().endswith("\\")
            continue
        for ch in ln:
            if ch == "{":
                st.append((header.strip(), hstart if hstart is not None else i, i)); header = ""; hstart = None
            elif ch == "}":
                if st:
                    h, hs, op = st.pop()
                    isfunc = ("(" in h and not re.match(notfunc, h) and not re.match(r"^(else\s+)?(if|for|while|switch|do|try|catch)\b", h)
                              and not h.startswith(("else", "do", "try")) and "=" not in h.split("(")[0])
                    if isfunc and not any(("(" in x[0] and not re.match(notfunc, x[0])) for x in st): spans.append((hs, op, i, h))
                header = ""; hstart = None
            elif ch == ";":
                header = ""; hstart = None
            else:
                if header.strip() == "" and ch.strip(): hstart = i
                header += ch
        header += " "
    spans.sort()
    return spans


def guess_kind(lines):
    """A guess of what the U lines of a function are, from their text only; a human confirms or corrects it in `verdict`."""
    txt = "\n".join(lines)
    kinds = []
    if re.search(r"\bRELEASE_ASSERT|\bCRASH\(|RELEASE_ASSERT_NOT_REACHED|breakpoint\(\)", txt): kinds.append("fail-stop")
    if re.search(r"\bASSERT|\bASSERT_", txt): kinds.append("debug-assert")
    if re.search(r"exchangeOr|exchangeAnd|exchangeAdd|atomicExchange|compareExchange|atomicCompareExchange|fetch_(add|or|and|sub)|\bcasWeak|\bcasStrong|atomicXchg", txt): kinds.append("locked-rmw")
    if re.search(r"\bLocker\b|\.lock\(\)|\bLockHolder|MutatorSlowPathLocker|GILOffCompilationLocker|Locker<", txt): kinds.append("lock")
    if re.search(r"storeStoreFence|loadLoadFence|storeLoadFence|fullFence|std::atomic_thread_fence", txt): kinds.append("fence")
    if re.search(r"atomicLoad|atomicStore|loadRelaxed|storeRelaxed|concurrentRelaxed|Concurrently\(|Concurrent\(|relaxedLoad|relaxedStore|std::memory_order", txt): kinds.append("relaxed-atomic")
    if re.search(r"orRelaxedNoLockedRMW|\bracyLoad|\bracyStore|ThreadsMode\w*|threadsMode\w+|\bsetFlag\(|\bclearFlag\(", txt): kinds.append("main-form-helper")
    if re.search(GATE, txt): kinds.append("gate-test")
    return ",".join(kinds) if kinds else "other"


# Verdicts the tool can decide from the text alone, for a row that has none: (verdict, reason). A human overrides any of them, and a
# row whose verdict a human has written is never touched. `reviewer` is "tool" for these, so the count of rows a person has looked at
# stays visible.
AUTO_VERDICT = {
    "gate-test": ("gate", "an inserted gate test: one predicted-false byte test of a frozen Config or option byte flag off"),
    "debug-assert": ("debug-assert", "assertion only: no release-build code"),
    "fail-stop": ("fail-stop", "a RELEASE_ASSERT or crash that flag off never reaches with the invariant it states"),
    "main-form-helper": ("restored", "a helper whose flag-off form is main's instruction sequence (plain access, unlocked, or a relaxed load and store)"),
}


def auto_verdict(kind):
    parts = kind.split(",")
    if all(p in ("gate-test", "debug-assert") for p in parts):
        return AUTO_VERDICT["gate-test"] if "gate-test" in parts else AUTO_VERDICT["debug-assert"]
    if kind in AUTO_VERDICT: return AUTO_VERDICT[kind]
    return None


def load_ledger(path):
    rows = {}
    if path and os.path.exists(path):
        for line in open(path, errors="replace"):
            if line.startswith("#") or not line.strip(): continue
            p = line.rstrip("\n").split("\t")
            if p[0] == "file": continue
            p += [""] * (8 - len(p))
            rows[(p[0], p[1])] = {"u": p[2], "auto": p[3], "verdict": p[4], "reason": p[5], "reviewer": p[6], "note": p[7]}
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default=os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))
    ap.add_argument("--base", required=True, help="the rev the branch is rebased on")
    ap.add_argument("--head", default=None, help="a rev (default: the working tree)")
    ap.add_argument("--ledger", default="docs/threads/FLAG-OFF-LEDGER.tsv")
    ap.add_argument("--summary", action="store_true", help="print the counts of FLAG-OFF-LANDING 2.4.1 and exit")
    ap.add_argument("--check", action="store_true", help="fail if a function with ungated lines has no row or a row has no verdict")
    ap.add_argument("--update", action="store_true", help="rewrite the ledger: new rows are added empty, rows of functions that no longer qualify are dropped")
    ap.add_argument("--strict-verdicts", action="store_true", help="with --check: also fail for rows without a verdict (default: report them)")
    ap.add_argument("--json", help="write per-line classes to this file")
    a = ap.parse_args()
    repo = a.repo
    headrev = a.head
    diff = git(repo, "diff", "--no-color", "-U0", "--no-renames", a.base, *([headrev] if headrev else []), "--", *SOURCE_DIRS)
    files = []; cur = None
    for l in diff.split("\n"):
        if l.startswith("diff --git"):
            cur = {"path": l.split(" b/")[-1], "hunks": [], "new": False}; files.append(cur)
        elif l.startswith("new file mode") and cur: cur["new"] = True
        elif l.startswith("@@") and cur is not None:
            m = re.match(r"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@", l)
            cur["hunks"].append({"new": int(m.group(3)), "lines": []})
        elif cur is not None and cur["hunks"] and l[:1] in "+-" and not l.startswith(("+++", "---")):
            cur["hunks"][-1]["lines"].append(l)
    clf = Classifier()
    counts = collections.Counter(); perline = []; funcs = {}
    hunk_kinds = collections.Counter(); nfiles_u = set()
    for f in files:
        path = f["path"]
        if headrev: text = git(repo, "show", headrev + ":" + path)
        else:
            try: text = open(os.path.join(repo, path), errors="replace").read()
            except OSError: continue                    # deleted
        removed = set(re.sub(r"\s+", "", l[1:]) for h in f["hunks"] for l in h["lines"] if l.startswith("-"))
        added = []; added_text = {}
        for h in f["hunks"]:
            n = h["new"]
            for l in h["lines"]:
                if l.startswith("+"): added.append(n); added_text[n] = l[1:]; n += 1
        res, code = clf.classify_file(path, text, added, removed)
        for n in added: counts[res[n]] += 1
        for h in f["hunks"]:
            c = collections.Counter(res[n] for n in range(h["new"], h["new"] + sum(1 for l in h["lines"] if l.startswith("+"))))
            kinds = {k for k in c if k != "C"}
            if not c: k = "removal-only"
            elif not kinds: k = "comment-only"
            elif kinds <= {"G+", "N"}: k = "flag-on-only"
            elif kinds <= {"G+", "N", "M", "G-"}: k = "flag-on + moved/flag-off-arm"
            elif kinds <= {"D"}: k = "declarations-only"
            elif "U" in kinds: k = "has-ungated-code"
            else: k = "mixed-decl"
            hunk_kinds[k] += 1
        ulines = [n for n in added if res[n] in ("U", "G-")]
        if not ulines: continue
        spans = function_spans(code) if path.endswith(C_LIKE) else []
        addset = set(added)
        for n in ulines:
            idx = n - 1; fn = None
            for hs, op, cl, h in spans:
                if hs <= idx <= cl: fn = (hs, op, cl, h)
            if not path.endswith(C_LIKE): key = (path, "(non-C file)"); kind = "EXISTING"
            elif fn is None: key = (path, "(no enclosing function found)"); kind = "EXISTING"
            else:
                hs, op, cl, h = fn
                key = (path, re.sub(r"\s+", " ", h)[:160])
                kind = "NEW" if all((k + 1) in addset for k in range(hs, op + 1) if code[k].strip()) else "EXISTING"
            e = funcs.setdefault(key, {"kind": kind, "lines": [], "classes": collections.Counter()})
            e["lines"].append(added_text[n]); e["classes"][res[n]] += 1
            perline.append((path, n, res[n], kind))
        nfiles_u.add(path)
    if a.json:
        json.dump(perline, open(a.json, "w"))
    existing = {k: v for k, v in funcs.items() if v["kind"] == "EXISTING" and k[1] != "(non-C file)" and v["classes"].get("U")}
    newf = {k: v for k, v in funcs.items() if v["kind"] == "NEW"}
    non_c = {k: v for k, v in funcs.items() if k[1] == "(non-C file)"}
    ex_lines = sum(v["classes"]["U"] for v in existing.values())
    # of the ungated lines in existing functions: inserted gate tests, trivial lines, other
    gate_lines = trivial = other = 0; fn_other = 0
    for v in existing.values():
        has_other = False
        for l in v["lines"]:
            s = re.sub(r"\[\[\w+\]\]", "", l).strip()
            if re.fullmatch(r"[{}();,]*|else\s*\{?|\}\s*else\s*\{?|\)\s*\{?", s): trivial += 1
            elif re.search(GATE_MENTION, l): gate_lines += 1
            else: other += 1; has_other = True
        fn_other += has_other
    if a.summary or not (a.check or a.update):
        print("added lines by class:", {k: counts[k] for k in ("C", "N", "M", "D", "G+", "G-", "U")})
        print("hunks:", sum(hunk_kinds.values()), dict(hunk_kinds))
        print("EXISTING functions with ungated (U) lines: %d functions, %d lines, %d files" % (len(existing), ex_lines, len({k[0] for k in existing})))
        print("  of those lines: %d gate tests or mentions of a gate, %d trivial (braces, else), %d other in %d functions" % (gate_lines, trivial, other, fn_other))
        print("NEW functions with ungated lines: %d (%d lines)" % (len(newf), sum(v["classes"]["U"] + v["classes"]["G-"] for v in newf.values())))
        print("non-C files with added lines: %d files" % len({k[0] for k in non_c}))
        by_auto = collections.Counter()
        for v in existing.values(): by_auto[guess_kind(v["lines"])] += 1
        print("rows by guessed kind:", dict(by_auto.most_common(12)))
    ledger_path = os.path.join(repo, a.ledger)
    rows = load_ledger(ledger_path)
    if a.update:
        out = ["file\tfunction\tungated_lines\tauto\tverdict\treason\treviewer\tnote"]
        for k in sorted(existing):
            v = existing[k]; old = rows.get(k, {})
            kind = guess_kind(v["lines"]); verdict, reason, reviewer = old.get("verdict", ""), old.get("reason", ""), old.get("reviewer", "")
            if not verdict and auto_verdict(kind):
                verdict, reason = auto_verdict(kind); reviewer = "tool"
            out.append("\t".join([k[0], k[1], str(v["classes"]["U"]), kind, verdict, reason, reviewer, old.get("note", "")]))
        header = ["# Generated by Tools/threads/flagoff-ledger.py (--base %s). One row per pre-existing function that has code the branch added" % a.base,
                  "# outside every flag-on gate. Columns verdict, reason, reviewer are maintained by hand and survive regeneration.",
                  "# verdict: shape | restored | deliberate | data | fail-stop | lock | atomic | upstream-fix | flag-on-arm (see docs/threads/FLAG-OFF-LANDING.md L3)."]
        open(ledger_path, "w").write("\n".join(header + out) + "\n")
        newrows = load_ledger(ledger_path)
        print("ledger written: %d rows, %d with a verdict (%d by the tool, %d by a reviewer)" % (
            len(existing), sum(1 for k in existing if newrows.get(k, {}).get("verdict")),
            sum(1 for k in existing if newrows.get(k, {}).get("reviewer") == "tool"),
            sum(1 for k in existing if newrows.get(k, {}).get("verdict") and newrows.get(k, {}).get("reviewer") not in ("", "tool"))))
    if a.check:
        missing = [k for k in existing if k not in rows]
        undecided = [k for k in existing if k in rows and not rows[k]["verdict"]]
        print("ledger check: %d functions, %d without a row, %d rows without a verdict" % (len(existing), len(missing), len(undecided)))
        for k in missing[:40]: print("  no row:", k[0], "|", k[1])
        if missing or (undecided and a.strict_verdicts): sys.exit(1)


if __name__ == "__main__":
    main()
