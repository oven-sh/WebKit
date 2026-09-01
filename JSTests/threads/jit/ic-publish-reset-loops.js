//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// ic-publish-reset-loops.js — SPEC-jit Task 13: I6 inlined fast-path stress
// ("flip an IC between two structures under readers") + §5.1 handler
// publish/reset loops (retireHandlerChain via resetStubAsJumpInAccess).
//
// I6 (structural, §4.2): the packed {byIdSelfOffset, structureID} word must
// never be observable as a valid structure id with a mismatched offset. The
// two shapes below place `f` at DIFFERENT offsets with DISTINCT values, and
// every neighboring slot holds a poison value — so a torn id/offset pair
// would read a poison and fail the membership assert.
//
// Under the phase-1 GIL the spawned readers interleave cooperatively
// (GIL-interleaved per the task charter); post-GIL the same file is a true
// concurrent reader stress.

load("../harness.js", "caller relative");

const POISON = 0xdead;
const VALUE_A = 11;
const VALUE_B = 22;
const EXPECTED = new Set([VALUE_A, VALUE_B]);

// Shape A: f out-of-line, late offset; neighbors poisoned.
function makeA() {
    const o = {};
    for (let i = 0; i < 10; ++i)
        o["a" + i] = POISON;
    o.f = VALUE_A;
    o.aTail = POISON;
    return o;
}

// Shape B: f early (different offset than A's); neighbors poisoned.
function makeB() {
    const o = {};
    o.f = VALUE_B;
    for (let i = 0; i < 10; ++i)
        o["b" + i] = POISON;
    return o;
}

function getF(o) { return o.f; }
noInline(getF);

function putF(o, v) { o.f = v; }
noInline(putF);

const stableA = makeA();
const stableB = makeB();

// Warm the IC on both shapes (publishes the packed self word for one and a
// handler chain node for the other).
for (let i = 0; i < 10000; ++i) {
    if (getF(stableA) !== VALUE_A || getF(stableB) !== VALUE_B)
        throw new Error("warmup mismatch");
}

let stop = { value: false };
let readerFailures = { count: 0 };

const readers = (typeof Thread === "function") ? spawnN(3, function (index) {
    let reads = 0;
    while (!stop.value) {
        const va = getF(stableA);
        const vb = getF(stableB);
        if (!EXPECTED.has(va) || !EXPECTED.has(vb)) {
            readerFailures.count++;
            throw new Error("reader " + index + " observed torn IC state: " + va + "/" + vb);
        }
        ++reads;
        if (!(reads % 256))
            sleepMs(0); // cooperative-GIL yield so the flipper interleaves
    }
    return reads;
}) : [];

// Flipper loop: hammer publish (fresh shapes keep prepending handlers /
// re-publishing the packed word) and reset (dictionary transitions force
// resetStubAsJumpInAccess => flag-on retireHandlerChain; GC pressure drives
// finalizeUnconditionally resets too).
const haveDollarVM = typeof $vm !== "undefined";
for (let round = 0; round < 200; ++round) {
    // Publish churn: alternate fresh instances of both shapes.
    for (let i = 0; i < 200; ++i) {
        const o = (i & 1) ? makeA() : makeB();
        const v = getF(o);
        if (!EXPECTED.has(v))
            throw new Error("flipper observed torn IC state: " + v);
        putF(o, (i & 1) ? VALUE_A : VALUE_B); // replace-only writes (same value domain)
        if (!EXPECTED.has(getF(o)))
            throw new Error("flipper put/get mismatch");
    }
    // Reset churn: force IC resets via dictionary round-trips on throwaway
    // instances of the SAME structures the IC has cached.
    if (haveDollarVM && $vm.toCacheableDictionary) {
        const victim = (round & 1) ? makeA() : makeB();
        getF(victim);
        $vm.toCacheableDictionary(victim);
        if (getF(victim) !== ((round & 1) ? VALUE_A : VALUE_B))
            throw new Error("post-dictionary read wrong");
        if ($vm.flattenDictionaryObject)
            $vm.flattenDictionaryObject(victim);
    }
    if (haveDollarVM && !(round % 50))
        $vm.gc(); // sweeps + finalizeUnconditionally reset paths
    if (typeof Thread === "function" && !(round % 20))
        sleepMs(1); // drop the GIL so readers interleave mid-churn
}

stop.value = true;
if (readers.length) {
    const reads = joinAll(readers);
    shouldBe(readerFailures.count, 0);
    for (const r of reads)
        shouldBeTrue(r > 0, "every reader must have completed reads");
}

// Final coherence.
shouldBe(getF(stableA), VALUE_A);
shouldBe(getF(stableB), VALUE_B);
print("ic-publish-reset-loops: PASS");
