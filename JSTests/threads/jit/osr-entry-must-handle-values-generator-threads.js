//@ requireOptions("--useJSThreads=1", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForFTLOptimizeAfterWarmUp=100")
// OSR-entry compiles carry a raw snapshot of the triggering frame
// (mustHandleValues) to the compiler thread, which decodes the Structure of
// every cell in it (DFG::AbstractValue::mergeOSREntryValue,
// TypeCheckHoistingPhase). A generator body returns at every yield, so the
// cells its loop header holds are reachable only through the plan's GC visit
// once the snapshot is taken; nothing in the snapshot may ever be a non-cell
// masquerading as one. Flag-on, an FTL direct tail call (next() ->
// @generatorResume) once replaced the resume-state argument with a raw
// DirectCallLinkInfo*, which then reached the body's loop-header snapshot
// and was decoded as a cell. Drive hot generators from several threads, with
// object/array/string cells live at the loop header, through enough resumes
// for next() and the body to reach the optimizing tiers.
load("../harness.js", "caller relative");

const THREADS = 4;
const OUTER = 30;
const YIELDS = 300;

function* g(n, seed) {
    let acc = { v: seed, arr: [seed], tag: "s" + seed };
    for (let i = 0; i < n; ++i) {
        acc = { v: acc.v + i, arr: [i, acc.arr.length], tag: acc.tag };
        yield acc;
    }
    return acc;
}

function worker(seed) {
    let total = 0;
    for (let outer = 0; outer < OUTER; ++outer) {
        const gen = g(YIELDS, seed);
        let count = 0;
        let expectedV = seed;
        for (;;) {
            const r = gen.next();
            if (typeof r !== "object" || r === null)
                throw new Error("seed=" + seed + " outer=" + outer + " torn IteratorResult: " + String(r));
            if (r.done)
                break;
            const value = r.value;
            if (typeof value !== "object" || value === null || value.arr.length !== 2 || value.arr[0] !== count || value.arr[1] !== 2 - (count === 0 ? 1 : 0))
                throw new Error("seed=" + seed + " outer=" + outer + " yield " + count + " carried a bad value: " + JSON.stringify(value));
            expectedV += count;
            if (value.v !== expectedV)
                throw new Error("seed=" + seed + " outer=" + outer + " yield " + count + " v=" + value.v + " expected " + expectedV);
            if (value.tag !== "s" + seed)
                throw new Error("seed=" + seed + " outer=" + outer + " yield " + count + " tag=" + value.tag);
            total += value.arr[0];
            ++count;
        }
        if (count !== YIELDS)
            throw new Error("seed=" + seed + " outer=" + outer + " early completion: count=" + count);
        const post = gen.next();
        if (post.done !== true || post.value !== undefined)
            throw new Error("seed=" + seed + " outer=" + outer + " resurrected after completion");
    }
    return total;
}

const threads = spawnN(THREADS, worker);
const mainTotal = worker(THREADS);
const results = joinAll(threads);
const expectedTotal = OUTER * (YIELDS * (YIELDS - 1) / 2);
shouldBe(mainTotal, expectedTotal);
for (let t = 0; t < THREADS; ++t)
    shouldBe(results[t], expectedTotal);
