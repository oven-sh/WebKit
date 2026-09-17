//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--useFTLJIT=false")
// SPEC-jit §5.8, history §58 (tenth round). The republication of a polymorphic call
// stub at a callee's install must not run on a thread doing the collector's work:
// the End phase jettisons dead code block edges and reinstalls their alternatives
// through the same incoming-calls drain, where a routine may neither be allocated
// nor run write barriers, and where callers the collection has just found dead
// are still on their callees' lists. The first build republished there and pushed
// a dead CodeBlock onto the mark stack: stress/array-shift-intrinsic.js crashed in
// the collector (SIGSEGV in SlotVisitor::visitChildren; Debug: "ASSERTION FAILED:
// isMarked(cell)" in the write barrier) in the three configurations of the GIL-off
// JSC suite that run it without the FTL, every time. This runs that file the same
// way: many short-lived closures calling through the same polymorphic sites while
// their code blocks die. It passes by finishing.
// (An assertion-enabled build runs the file's ten thousand rounds per case past the 120 s limit; a fraction is plenty there.)
if ($vm.assertEnabled && $vm.assertEnabled())
    globalThis.testLoopCount = 300;
load("../../stress/array-shift-intrinsic.js", "caller relative");
