//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--countJSThreadsCounters=1")
// DESIGN-PROPOSALS sections A (A-3) and K. Characterises current behaviour; written in the design session that
// followed the tenth round, before any fix.
//
// GIL off, when a function's installed replacement is an FTL function-entry block, a Baseline loop enters the DFG
// block that the FTL superseded (kept for loop entry since the eighth round) and, through that block's loop tier-up,
// its FTL-for-OSR-entry child. operationOptimize decides whether to reoptimize from the REPLACEMENT's exit counter,
// but the exits of the two blocks actually entered are counted on their own counters, so the loop reoptimization
// trigger never fires for them: each is left only through its own exit stub's non-loop threshold,
// 100 x (1 + live spawned threads) x 2^retry, with a long Baseline warm-up after every exit.
//
// Here: three invocations of one function whose hot loop reads objs[i].a; each invocation builds its objects with
// another property order, so the code compiled during one invocation fails its first structure check in the next.
// main, flag off and GIL on take 2 BadCache exits in the third invocation. GIL off (Release, final tree of the tenth
// round) the third invocation enters the superseded DFG 801 times and takes 903 exits (--printEachOSRExit), every
// run, and runs about twice as long. On a Debug build the FTL replacement is usually not installed yet at this
// iteration count and the counter stays 0.
//
// The bound below only keeps the pathology from growing. When the fix lands (the loop trigger applied to the block
// that is about to be entered) tighten it to a handful (the loop threshold, 5 x (1 + live threads) x 2^retry, is
// the most the fixed engine should need).
function work(tag, steps) {
    function mk(i) {
        if (tag === 0) return { a: i, b: 0, c: 0 };
        if (tag === 1) return { b: 0, a: i, c: 0 };
        return { c: 0, b: 0, a: i };
    }
    const objs = [mk(1), mk(2), mk(3), mk(4), mk(5)];
    let s = 0;
    for (let step = 0; step < steps; ++step) {
        for (let i = 0; i < objs.length; ++i)
            s = (s + objs[i].a + step) | 0;
    }
    return s;
}
noInline(work);

const N = 1000000;
function expected(steps) {
    let s = 0;
    for (let step = 0; step < steps; ++step) {
        for (let i = 1; i <= 5; ++i)
            s = (s + i + step) | 0;
    }
    return s;
}
const want = expected(N);

if (work(0, N) !== want || work(1, N) !== want)
    throw new Error("wrong result in the warm-up invocations");

const before = $vm.jsThreadsCounter("loopEntryIntoSupersededDFGGILOff");
const result = new Thread(() => work(2, N)).join();
if (result !== want)
    throw new Error("third invocation returned " + result + ", expected " + want);
const entries = $vm.jsThreadsCounter("loopEntryIntoSupersededDFGGILOff") - before;

// Today: 0 with the GIL on (the path is GIL-off only), 801 GIL off on a Release build.
if (entries > 2000)
    throw new Error("the third invocation entered the superseded DFG block " + entries + " times");
