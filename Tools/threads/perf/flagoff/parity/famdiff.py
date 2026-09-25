#!/usr/bin/env python3
"""famdiff.py <main .fn or dir> <branch .fn or dir>: the instruction difference by code family."""
import glob, os, re, collections, sys
def load(d):
    c = collections.Counter()
    for p in ([d] if os.path.isfile(d) else glob.glob(d + '/*.fn')):
        for l in open(p, errors='replace'):
            n, _, f = l.rstrip('\n').partition('\t')
            f = re.sub(r"'\d+$", '', f.strip())
            if n.isdigit() and f != 'PROGRAM TOTALS': c[f] += int(n)
    return c
FAM = [
 ("generated code", r"^0x0000[0-9a-f]{4}[0-9a-f]{8}$(?<!0x0000000000[0-9a-f]{6})|^\[unknown\]"),
 ("libc and other libraries (by address)", r"^0x0000000000[0-9a-f]{6}$"),
 ("DFG/FTL/B3/Air compilers", r"DFG::|FTL::|B3::|Air::"),
 ("Baseline JIT, ICs, linking, assembler", r"JSC::JIT::|JITPlan|JITWorklist|InlineCacheCompiler|PropertyInlineCache|Repatch|repatch|CallLinkInfo|LinkBuffer|MacroAssembler|AssemblyHelpers|CCallHelpers|ExecutableAllocator|MetaAllocator|JITCode|Thunk|AccessCase|PolymorphicAccess|JITStubRoutine|StructureStubInfo|X86Assembler|AssemblerBuffer"),
 ("Yarr", r"Yarr"),
 ("LLInt asm", r"^llint_(?!slow_path)|^op_|^vmEntry|^iterator_|^js_trampoline"),
 ("LLInt and common slow paths", r"slow_path|LLInt::"),
 ("parser, bytecode generator, linking", r"Parser|Lexer|ASTBuilder|SyntaxChecker|BytecodeGenerator|Node::|emitBytecode|UnlinkedCodeBlock|UnlinkedFunction|CodeCache|generateUnlinked|ScriptExecutable|FunctionExecutable|CodeBlock::finishCreation|CodeBlock::CodeBlock|BytecodeLiveness|SourceProvider|Identifier|VariableEnvironment|ProgramExecutable|ModuleAnalyzer|Scope::|BytecodeRewriter|InstructionStream|preciseJumpTargets|Bytecode"),
 ("collector: marking", r"SlotVisitor|visitChildren|visitButterfly|MarkStack|markAux|appendHidden|appendJSCell|visitWeak|ConservativeRoots|MarkingConstraint|reconcileWeak|finalizeUnconditional"),
 ("collector: sweeping, allocation slow paths, the rest", r"Heap::|MarkedBlock|MarkedSpace|BlockDirectory|LocalAllocator|Subspace|sweep|WeakSet|WeakBlock|IsoCellSet|FreeList|PreciseAllocation|WriteBarrier|RememberedSet|sanitizeStack|DeferGC|CellSet|GCClient|CompleteSubspace"),
 ("allocation fast paths, malloc", r"allocateCell|pas_|bmalloc|fastMalloc|fastFree|libpas|malloc|free|tryAllocate"),
 ("strings, atoms", r"JSString|JSRopeString|StringImpl|AtomString|StringBuilder|makeString|StringView|jsString|jsSubstring|Joiner|StringPrototype|stringProto|addToStringTable|NumericStrings|int32ToString|StringRecursion"),
 ("RegExp runtime", r"RegExp|regExp"),
 ("object model, arrays, structures", r"JSObject|JSArray|Structure|Butterfly|PropertyTable|putDirect|PropertySlot|JSFinalObject|JSFunction|JSGlobalObject|arrayProto|ArrayPrototype|ObjectConstructor|JSCell|JSValue|TypedArray|ArrayBuffer|HashMapImpl|OrderedHashTable|WeakGCMap|WeakMap|Symbol|JSON|Stringifier|LiteralParser|ObjectAllocationProfile|Watchpoint|GetterSetter|DirectArguments|LexicalEnvironment|JSMap|JSSet|constructSet|constructMap"),
 ("operations", r"^operation"),
 ("calls, exceptions, entry", r"Interpreter::|CallFrame|ExceptionScope|ThrowScope|VMEntry|VMTraps|JSLock|Microtask|Promise|StackVisitor|Arguments|Varargs"),
 ("other", r".*"),
]
def fam(s):
    if re.match(r'^0x0000000000[0-9a-f]{6}$', s): return "libc and other libraries (by address)"
    if re.match(r'^0x[0-9a-f]+$', s) or s == '[unknown]': return "generated code"
    for n, p in FAM[2:]:
        if re.search(p, s): return n
a = load(sys.argv[1]); b = load(sys.argv[2]); ta = sum(a.values())
fa = collections.Counter(); fb = collections.Counter()
for s, n in a.items(): fa[fam(s)] += n
for s, n in b.items(): fb[fam(s)] += n
print("branch / main = %.4f  (main %.3f G instructions)" % (sum(b.values()) / ta, ta / 1e9))
for n, _ in FAM:
    if fa[n] or fb[n]:
        print("  %-52s main %6.2f%%  diff %+7.3f%% of the total  (%+.2f%% of itself)" % (n, 100.0 * fa[n] / ta, 100.0 * (fb[n] - fa[n]) / ta, 100.0 * (fb[n] - fa[n]) / max(fa[n], 1)))
