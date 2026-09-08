//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--countJSThreadsCounters=1")
// SPEC-objectmodel T4-C (rev 18, history §27). GIL off, a fresh array made by
// copying a Double source (slice / concat / the DFG ArraySlice intrinsic) is
// Contiguous, so a use site that also sees Contiguous arrays does not convert
// every copy through a stop-the-world relabel. Before: ~one "OM relabel
// Double->Contiguous" stop per copy (176,389 for the 150,000 single-thread
// copies here, 0.81 s in the conductor); after: 0. Values, holes and lengths are checked
// in all modes; a second thread runs the same workload concurrently.
load("../harness.js", "caller relative");

function makeDouble(seed) { return [seed + 0.5, seed * 1.25, , 7.75]; } // Double literal with a hole
function makeGrown(seed) { const a = [seed | 0, 2, 3]; a[3] = seed + 0.5; return a; } // Int32 grown by a double: Contiguous GIL off, Double GIL on
function mixSlice(a) { let s = 0; for (let i = 0; i < a.length; ++i) { const v = a[i]; if (v !== undefined) s += v; } return s; }
function mixConcat(a) { let s = 0; for (let i = 0; i < a.length; ++i) { const v = a[i]; if (v !== undefined) s += v; } return s; }
function mixRange(a) { let s = 0; for (let i = 0; i < a.length; ++i) { const v = a[i]; if (v !== undefined) s += v; } return s; }
noInline(makeDouble); noInline(makeGrown); noInline(mixSlice); noInline(mixConcat); noInline(mixRange);

function run(rounds, tid) {
    let total = 0;
    const base = makeDouble(tid);
    for (let r = 0; r < rounds; ++r) {
        const c1 = base.slice(0);           // fastSlice / DFG ArraySlice
        total += mixSlice(c1) + mixSlice(makeGrown(r));
        const c2 = base.concat([r + 0.25]); // concat fast path
        total += mixConcat(c2) + mixConcat(makeGrown(r));
        const c3 = base.slice(1, 3);
        total += mixRange(c3) + mixRange(makeGrown(r));
        if (c1.length !== 4 || 2 in c1 || c1[3] !== 7.75 || c1[0] !== tid + 0.5) throw new Error("slice copy wrong: " + JSON.stringify(c1));
        if (c2.length !== 5 || c2[4] !== r + 0.25 || 2 in c2) throw new Error("concat copy wrong: " + JSON.stringify(c2));
        if (c3.length !== 2 || c3[0] !== tid * 1.25 || 1 in c3) throw new Error("slice(1,3) wrong: " + JSON.stringify(c3));
    }
    return total;
}
function expected(rounds, tid) {
    let total = 0;
    for (let r = 0; r < rounds; ++r) {
        const b = (tid + 0.5) + tid * 1.25 + 7.75, g = (r | 0) + 2 + 3 + r + 0.5;
        total += (b + g) + (b + r + 0.25 + g) + (tid * 1.25 + g);
    }
    return total;
}

const ROUNDS = 50000;
// Phase 1: the main thread alone (this is where the stop storm showed: with a
// second thread running, the stops' code invalidations let the sites
// re-profile and mostly converge, hiding the cost).
const stops0 = $vm.jsThreadsCounter("OM relabel Double->Contiguous");
const mine = run(ROUNDS, 0);
const stops = $vm.jsThreadsCounter("OM relabel Double->Contiguous") - stops0;
if (Math.abs(mine - expected(ROUNDS, 0)) > 1e-3) throw new Error("main total " + mine + " expected " + expected(ROUNDS, 0));
if (typeof AMPLIFY_VERBOSE !== "undefined") print("Double->Contiguous stops for " + (ROUNDS * 3) + " copies on one thread: " + stops);
if (stops > 3000) throw new Error(stops + " Double->Contiguous stop-the-world relabels for " + (ROUNDS * 3) + " fresh copies of a Double array");
// Phase 2: another thread alone, then both at once (correctness under sharing).
if (typeof Thread === "function") {
    const solo = new Thread(() => run(ROUNDS / 5, 1)).join();
    if (Math.abs(solo - expected(ROUNDS / 5, 1)) > 1e-3) throw new Error("thread total " + solo);
    const t = new Thread(() => run(ROUNDS / 5, 1));
    const again = run(ROUNDS / 5, 0);
    const theirs = t.join();
    if (Math.abs(again - expected(ROUNDS / 5, 0)) > 1e-3 || Math.abs(theirs - expected(ROUNDS / 5, 1)) > 1e-3) throw new Error("concurrent totals " + again + " " + theirs);
}
print("PASS");
