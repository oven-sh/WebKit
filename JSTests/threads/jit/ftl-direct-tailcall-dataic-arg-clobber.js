//@ requireOptions("--useJSThreads=1", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForFTLOptimizeAfterWarmUp=100")
// Regression test: flag-on FTL DirectTailCall with UseDataIC::Yes corrupted
// the argument (or the CallLinkRecord pointer) living in
// BaselineJITRegisters::Call::callLinkInfoGPR (regT2/%rdx on x86_64).
//
// SPEC-jit section 5.8 makes every flag-on direct call a data IC; the FTL
// lowering (FTLLowerDFGToB3.cpp compileDirectCallOrConstruct) reused the
// upstream UseDataIC::No patchpoint shape, which (a) let B3 assign the
// SomeRegister callee / WarmAny tail-call arguments to callLinkInfoGPR —
// stomped by emitDirectTailCallFastPath's move of the DirectCallLinkInfo*
// into that register BEFORE the CallFrameShuffler consumed the recoveries —
// and (b) never told the shuffler the record pointer was live across the
// shuffle. Mode (a) fed the link-info pointer to the callee as a JSValue
// argument; with GeneratorPrototype.js next() FTL-compiled, the boxed
// pointer arrived as generatorResume's `state`, the body's Int32
// speculation BadType-exited at entry, and baseline's switch_imm dispatched
// the garbage state to the ENTRY path: the generator silently restarted
// from its first yield on every FTL next() call (wrong values, no crash,
// GIL-on; SEGV / JIT-pool wild jump GIL-off, where mode (b) also corrupted
// the farJump target). Single-threaded, no concurrency required.
//
// The fix mirrors compileTailCall's protection for the non-direct data-IC
// path: clobberEarly(callLinkInfoGPR) + shuffleData.registers[
// callLinkInfoGPR] liveness. This test runs the exact discovery shape: a
// hot generator driven through enough resumes that next() reaches FTL, with
// per-call value checking and a post-completion resurrection check.

function* g(n) {
    for (let i = 0; i < n; ++i)
        yield i;
}

for (let outer = 0; outer < 200; ++outer) {
    const gen = g(2000);
    let count = 0;
    while (true) {
        const r = gen.next();
        if (r.done)
            break;
        if (r.value !== count)
            throw new Error("outer=" + outer + " yield mismatch: got " + r.value + " expected " + count + " (DirectTailCall argument corruption)");
        ++count;
    }
    if (count !== 2000)
        throw new Error("outer=" + outer + " early completion: count=" + count);
    const post = gen.next();
    if (post.done !== true || post.value !== undefined)
        throw new Error("outer=" + outer + " resurrected after completion: done=" + post.done + " value=" + post.value);
}
