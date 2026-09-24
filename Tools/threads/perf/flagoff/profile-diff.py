#!/usr/bin/env python3
# profile-diff.py <prefix> [-s N]: average the attribution dumps of $OUT/profile/<prefix>-main-* and <prefix>-off-*; print the difference by class
# (JIT tier) and by C++ family, as samples and as % of main's total.
import sys, json, re, collections, glob
import os
pre = sys.argv[1]; O = os.path.join(os.environ.get("OUT", "flagoff-out"), "profile") + "/"
FAM = [
 ("LLInt (interpreter asm and slow paths)", r"^llint_|LLInt::|^op_|slow_path_|CommonSlowPaths|vmEntryTo|^wasm_|ipint"),
 ("parser, bytecode generator, linking", r"Parser|Lexer|ASTBuilder|SyntaxChecker|BytecodeGenerator|Node::emit|emitBytecode|UnlinkedCodeBlock|UnlinkedFunction|CodeCache|CachedTypes|generateUnlinked|ScriptExecutable|FunctionExecutable|linkCodeBlock|CodeBlock::finishCreation|CodeBlock::CodeBlock|BytecodeLivenessAnalysis|SourceProvider|Identifier|VariableEnvironment|ProgramExecutable|ModuleAnalyzer|Scope::|BytecodeRewriter|InstructionStream|preciseJumpTargets|BytecodeBasicBlock|BytecodeGraph"),
 ("Baseline compiler, IC compilation, linking on the main thread", r"JIT::|BaselineJIT|JITPlan|JITWorklist|InlineCacheCompiler|PropertyInlineCache|StructureStubInfo|repatch|Repatch|CallLinkInfo|LinkBuffer|MacroAssembler|AssemblyHelpers|CCallHelpers|ExecutableAllocator|MetaAllocator|JITCode|JITThunks|ThunkGenerator|operationOptimize|operationLink|operationCompile|tierUp|DFG::|FTL::|B3::|Air::|OSR|Profile|prepareForExecution|installCode|jettison"),
 ("collector and allocation", r"Heap::|SlotVisitor|MarkedBlock|MarkedSpace|BlockDirectory|LocalAllocator|visitChildren|Subspace|allocateCell|tryCreate|sweep|WeakSet|WeakBlock|pas_|bmalloc|fastMalloc|fastFree|libpas|mi_|malloc|free|Allocator|IsoCellSet|FreeList|PreciseAllocation|GCThread|markAux|WriteBarrier|addToRememberedSet|sanitizeStack"),
 ("RegExp", r"RegExp|Yarr|regExp"),
 ("strings", r"JSString|JSRopeString|StringImpl|AtomString|StringBuilder|makeString|StringView|equal|toString|fromCharCode|charCodeAt|substring|jsSubstring|String::|WTF::String|Joiner|operationArrayJoin|replace|StringPrototype|stringProto"),
 ("object model and arrays (C++)", r"JSObject|JSArray|Structure|Butterfly|PropertyTable|putDirect|getOwnPropertySlot|PropertySlot|PropertyName|JSFinalObject|JSFunction|JSGlobalObject|arrayProto|ArrayPrototype|objectProto|ObjectConstructor|operationGet|operationPut|operationNew|operationCreate|operationIn|operationHas|operationArray|JSCell|JSValue|IndexingType|TypedArray|ArrayBuffer|JSMap|JSSet|HashMapImpl|OrderedHashTable|WeakMap|Symbol|Proxy|Reflect|JSON|Stringifier|LiteralParser"),
 ("calls, exceptions, entry", r"Interpreter::|CallFrame|ExceptionScope|ThrowScope|VMEntry|executeCall|executeProgram|VMTraps|JSLock|Microtask|Promise|Generator|AsyncFunction|operationThrow|unwind|genericUnwind|StackVisitor|ShadowChicken|setupVarargs|Arguments"),
 ("libc, kernel entry, unclassified", r".*"),
]
def fam(sym):
    for n, p in FAM:
        if re.search(p, sym): return n
def load(tag):
    fs = sorted(glob.glob(O + "%s-%s-*.json" % (pre, tag)))
    agg = {"tot": 0, "cls": collections.Counter(), "cpp": collections.Counter(), "names": collections.Counter()}
    for f in fs:
        d = json.load(open(f)); agg["tot"] += d["tot"]
        for k in ("cls", "cpp", "names"): agg[k].update(d[k])
    n = max(len(fs), 1)
    agg["tot"] /= n
    for k in ("cls", "cpp", "names"):
        for kk in agg[k]: agg[k][kk] /= n
    return agg, n
a, na = load("main"); b, nb = load("off")
T = a["tot"]
print("%s: runs main %d off %d; samples per run main %.0f off %.0f (off/main %.4f)" % (pre, na, nb, a["tot"], b["tot"], b["tot"] / a["tot"]))
print("-- by class (JIT tier or C++), difference as %% of main's total")
for k in sorted(set(a["cls"]) | set(b["cls"]), key=lambda k: -(b["cls"][k] - a["cls"][k])):
    print("  %-22s main %8.0f (%5.1f%%)  off %8.0f   diff %+7.0f  %+6.2f%%" % (k, a["cls"][k], 100 * a["cls"][k] / T, b["cls"][k], b["cls"][k] - a["cls"][k], 100 * (b["cls"][k] - a["cls"][k]) / T))
print("-- C++ by family")
fa = collections.Counter(); fb = collections.Counter()
for s, v in a["cpp"].items(): fa[fam(s)] += v
for s, v in b["cpp"].items(): fb[fam(s)] += v
for n, _ in FAM:
    print("  %-62s main %7.0f (%4.1f%%) off %7.0f diff %+6.0f %+5.2f%%" % (n, fa[n], 100 * fa[n] / T, fb[n], fb[n] - fa[n], 100 * (fb[n] - fa[n]) / T))
if "-s" in sys.argv:
    print("-- largest C++ symbol differences")
    rows = sorted(((b["cpp"][k] - a["cpp"][k], k) for k in set(a["cpp"]) | set(b["cpp"])), reverse=True)
    for dlt, k in rows[:int(sys.argv[sys.argv.index("-s") + 1])]: print("  %+7.0f  main %6.0f off %6.0f  [%s] %s" % (dlt, a["cpp"][k], b["cpp"][k], (fam(k) or "")[:12], k))
    print("  ...")
    for dlt, k in rows[-8:]: print("  %+7.0f  main %6.0f off %6.0f  %s" % (dlt, a["cpp"][k], b["cpp"][k], k))
