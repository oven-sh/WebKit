//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// int-gate-epoch-reclaim.js — SPEC-jit Task 13: epoch retirement tests
// (§4.4/I7/I9/I15/I16).
//
// Two variants in one file:
//
//  PRE-INTEGRATION (runs by default): retire -> legacy-GC -> free ordering.
//    Heap §11 reclaims retired items at legacy collection end, so with the
//    N6 shim live (GCSafepointEpoch.h landed) this loop actually frees the
//    retired handler chains; with the shim dark it leak-checks the no-op
//    stub (sound either way — the test's oracle is "no use-after-free crash
//    and values stay correct while old chains are still referenced by
//    in-flight dispatch").
//
//  INTEGRATION GATE (`-- int-gate`, at M4/CS2): retire -> safepoint ->
//    refcount/free ordering with a PARKED-IN-SLOW-PATH thread (§4.4(b)):
//    one thread sits in a native slow path holding a Ref to a handler
//    (I15) across the conductor's stops while the conductor retires that
//    handler's chain and crosses safepoints; the data must not be freed
//    until the Ref drops AND the epoch expires (I7: machine code only after
//    R2's scan).
//
// JS cannot observe frees directly; the oracle is crash-freedom under
// ASan/TSAN runs of this loop plus value correctness through the churn.

load("../harness.js", "caller relative");

const FULL = typeof arguments !== "undefined" && Array.prototype.indexOf.call(arguments, "int-gate") >= 0;
const ROUNDS = FULL ? 300 : 40;
const THREADS = FULL ? 3 : 1;

const VALUES = new Set([7, 13]);

function makeP(v) { const o = {}; o.p0 = 0xbad; o.f = v; o.p1 = 0xbad; return o; }
function makeQ(v) { const o = {}; o.f = v; o.q0 = 0xbad; o.q1 = 0xbad; o.q2 = 0xbad; return o; }

function getF(o) { return o.f; }
noInline(getF);

// Warm a polymorphic chain: two shapes => head + at least one chained node.
const stableP = makeP(7);
const stableQ = makeQ(13);
for (let i = 0; i < 10000; ++i) {
    if (getF(stableP) !== 7 || getF(stableQ) !== 13)
        throw new Error("warmup");
}

const stop = { value: false };
// Dispatchers: threads that KEEP CALLING through the IC while the conductor
// retires its chains — flag-on, retired chains must stay dereferenceable
// until every thread crossed a safepoint past the retire (epoch >= retire+1)
// AND refcounts drop (§4.4 hard rule).
const dispatchers = (typeof Thread === "function" && THREADS > 0) ? spawnN(THREADS, function (slot) {
    let n = 0;
    while (!stop.value) {
        const v = getF((n & 1) ? stableP : stableQ);
        if (!VALUES.has(v))
            throw new Error("dispatcher " + slot + " read through a freed/torn handler: " + v);
        ++n;
        if (!(n % 128))
            sleepMs(0); // safepoint/park opportunity (the epoch-crossing event)
    }
    return n;
}) : [];

for (let round = 0; round < ROUNDS; ++round) {
    // RETIRE: force resetStubAsJumpInAccess on the hot IC by dictionary
    // round-tripping a cached-structure instance (flag-on this routes the
    // displaced chain through RetiredJITArtifacts::retireHandlerChain, I9).
    if (typeof $vm !== "undefined" && $vm.toCacheableDictionary) {
        const victim = (round & 1) ? makeP(7) : makeQ(13);
        getF(victim);
        $vm.toCacheableDictionary(victim);
        if (getF(victim) !== ((round & 1) ? 7 : 13))
            throw new Error("post-dictionary value wrong");
        if ($vm.flattenDictionaryObject)
            $vm.flattenDictionaryObject(victim);
    }
    // Rebuild the chain so the next round has something to retire.
    for (let i = 0; i < 500; ++i) {
        if (!VALUES.has(getF((i & 1) ? stableP : stableQ)))
            throw new Error("rebuild read bad value");
    }
    // LEGACY-GC RECLAIM (pre-integration variant): heap §11 frees expired
    // retired items at collection end; eden + full mix.
    if (typeof $vm !== "undefined") {
        if (round & 1)
            $vm.edenGC();
        else
            $vm.gc();
    }
    if (typeof Thread === "function" && !(round % 10))
        sleepMs(1); // let dispatchers cross their poll sites (epoch advance)
}

stop.value = true;
if (dispatchers.length) {
    for (const n of joinAll(dispatchers))
        shouldBeTrue(n > 0, "dispatcher progressed through retire/GC churn");
}
shouldBe(getF(stableP), 7);
shouldBe(getF(stableQ), 13);
print("int-gate-epoch-reclaim: PASS (" + (FULL ? "FULL" : "pre-integration legacy-GC variant; rerun with -- int-gate at M4/CS2") + ")");
