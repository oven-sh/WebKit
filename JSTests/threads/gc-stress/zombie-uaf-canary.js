//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// zombie-uaf-canary.js — gc-stress suite: allocate/drop/reallocate shapes
// engineered so that any STALE pointer retained by the engine (the
// ic-publish UAF family: IC stubs, handler chains, watchpoint nodes, or
// cached butterflies pointing at swept cells) lands in a REUSED cell whose
// new contents are a recognizable canary.
//
// NOTE ON VALUE: run standalone this test is a generation-churn smoke. Its
// real VALUE is under Tools/threads/gc-stress-matrix.sh in the `scribble`
// (--scribbleFreeCells=1) and `zombie` (--useZombieMode=1) modes: there the
// sweep itself poisons freed cells (zombie mode scribbles 0xbadbeef0), so a
// single stale-pointer dereference — even one that would otherwise
// accidentally read a still-plausible value from the reused cell — returns
// poison and trips the domain asserts or crashes immediately at the buggy
// dereference rather than corrupting state silently.
//
// Mechanics:
//   - Two fixed shapes (A: `f` late/out-of-line, B: `f` early) with DISTINCT
//     values and poisoned neighbor slots, exactly the ic-publish family
//     shape: a torn or stale {structureID, offset} pair reads a neighbor.
//   - Hot noInline'd get/put fast paths warmed on both shapes (publishes IC
//     state holding structure/offset/possibly-cell pointers).
//   - Generations: allocate a wave of A/B instances, run the fast paths over
//     all of them, then DROP the entire wave, $vm.gc(), and immediately
//     reallocate a same-size wave of CANARY objects (same slot counts, every
//     field = 0xc0de) so freed cells are reoccupied with canary bits.
//   - Two churn threads do the same allocate/drop dance concurrently so the
//     allocator's per-thread reuse paths are exercised, not just main's.
//   - Long-lived survivors are re-verified after every generation: any IC,
//     handler, or butterfly pointer that survived the sweep and got used
//     produces a value outside {VALUE_A, VALUE_B} (canary 0xc0de, neighbor
//     poison 0xdead, or zombie scribble) and fails loudly.
//
//   - A structure-death phase follows: per-generation UNIQUE property names
//     and a per-generation noInline'd accessor, so warmed Structures
//     actually die (nothing pins them), cells are canary-reoccupied, and
//     the surviving accessor is re-exercised against a rebuilt same-named
//     shape — covering the stub-outlives-structure / recycled-StructureID
//     family the survivor-pinned generation loop cannot reach.
//
// Runtime: bounded — 24 generations x 300-object waves, 8 structure-death
// generations x 64-object waves, 2 churn threads stopped via Atomics gate.

load("../harness.js", "caller relative");

const POISON = 0xdead;
const CANARY = 0xc0de;
const VALUE_A = 31;
const VALUE_B = 47;
const EXPECTED = new Set([VALUE_A, VALUE_B]);
const GENERATIONS = 24;
const WAVE = 300;
const CHURN_THREADS = 2;

// Shape A: f out-of-line at a late offset; all neighbors poisoned.
function makeA() {
    const o = {};
    for (let i = 0; i < 10; ++i)
        o["a" + i] = POISON;
    o.f = VALUE_A;
    o.aTail = POISON;
    return o;
}

// Shape B: f at an early (different) offset; neighbors poisoned after.
function makeB() {
    const o = {};
    o.f = VALUE_B;
    for (let i = 0; i < 10; ++i)
        o["b" + i] = POISON;
    return o;
}

// Canary reallocation: same slot count as A/B so freed cells of those sizes
// are reoccupied; every field (including one named f!) holds the canary.
function makeCanary() {
    const o = {};
    for (let i = 0; i < 10; ++i)
        o["c" + i] = CANARY;
    o.f = CANARY;
    o.cTail = CANARY;
    return o;
}

function getF(o) { return o.f; }
noInline(getF);
function putF(o, v) { o.f = v; }
noInline(putF);

// Warm the IC on both shapes.
const survivors = [];
for (let i = 0; i < 8; ++i)
    survivors.push((i & 1) ? makeA() : makeB());
for (let i = 0; i < 10000; ++i) {
    const v = getF(survivors[i & 7]);
    if (!EXPECTED.has(v))
        throw new Error("warmup mismatch: " + v);
}

function verifySurvivors(where) {
    for (let i = 0; i < survivors.length; ++i) {
        const v = getF(survivors[i]);
        const expected = (i & 1) ? VALUE_A : VALUE_B;
        if (v !== expected)
            throw new Error(where + ": survivor " + i + " read " + describe(v)
                + " (expected " + expected + "; " + CANARY + "=canary, "
                + POISON + "=neighbor poison, large negative/0xbadbeef0-family=zombie scribble)");
    }
}

// Concurrent churn threads: same allocate/use/drop pattern, fixed bounds, so
// cell reuse also flows through whatever per-thread allocator state exists.
const gate = { started: 0, stop: 0 };
const churners = spawnN(CHURN_THREADS, function (index) {
    Atomics.add(gate, "started", 1);
    let waves = 0;
    let canaryFold = 0;
    while (Atomics.load(gate, "stop") === 0) {
        let wave = [];
        for (let i = 0; i < WAVE; ++i) {
            const o = (i & 1) ? makeA() : makeB();
            const v = getF(o);
            if (!EXPECTED.has(v))
                throw new Error("churner " + index + " observed " + describe(v));
            wave.push(o);
        }
        wave = null;
        ++waves;
        // Immediate canary reoccupation pressure. The canaries must stay
        // OBSERVABLE — pushed into an escaping array with one field folded
        // into the thread's return value — so DFG/FTL allocation sinking /
        // DCE cannot legally elide the allocations once this loop tiers up
        // (a bare discarded makeCanary() would be eligible, silently
        // removing the reoccupation the test depends on).
        let cw = [];
        for (let i = 0; i < WAVE; ++i)
            cw.push(makeCanary());
        canaryFold += cw[WAVE - 1].f;
        cw = null;
        sleepMs(0); // cooperative-GIL yield
    }
    return { waves: waves, canaryFold: canaryFold };
});

// Started-rendezvous: both churners must be running before the fixed-bound
// generation loop, so the waves > 0 assertions are deterministic instead of
// depending on thread-startup scheduling luck under the cooperative GIL.
waitUntil(() => Atomics.load(gate, "started") === CHURN_THREADS, 30000);

const haveDollarVM = typeof $vm !== "undefined";
for (let gen = 0; gen < GENERATIONS; ++gen) {
    // Allocate a wave and run the published fast paths over every member
    // (each access can publish/refresh IC state referencing these cells).
    let wave = [];
    for (let i = 0; i < WAVE; ++i) {
        const o = (i & 1) ? makeA() : makeB();
        putF(o, (i & 1) ? VALUE_A : VALUE_B); // replace-only, same domain
        const v = getF(o);
        if (!EXPECTED.has(v))
            throw new Error("gen " + gen + ": wave member read " + describe(v));
        wave.push(o);
    }

    // Drop the whole wave and sweep. Under matrix scribble/zombie modes the
    // freed cells are poisoned right here.
    wave = null;
    if (haveDollarVM) {
        $vm.gc();
        if (!(gen % 6) && $vm.edenGC)
            $vm.edenGC();
    }

    // Reoccupy: canary objects of the same sizes land in the freed cells.
    let canaries = [];
    for (let i = 0; i < WAVE; ++i)
        canaries.push(makeCanary());

    // Any stale pointer used now reads canary/poison/scribble, not A/B.
    verifySurvivors("gen " + gen + " post-reuse");
    canaries = null;

    sleepMs(0); // let churners run between generations
}

// ---- structure-death phase ----
// The generation loop above never lets a Structure die: the survivors pin
// shapes A and B for the whole run, so its coverage is allocator reuse and
// torn {structureID, offset} publishes — NOT stub-outlives-structure. This
// phase makes shapes actually DIE while a warmed accessor (and therefore its
// published IC stub / handler-chain state) survives:
//   - each dead-generation uses a generation-UNIQUE property name set, with
//     its own noInline'd accessor warmed on that generation's Structure;
//   - every referencing object is then dropped (nothing pins the Structure),
//     $vm.gc() sweeps, and canaries reoccupy the freed cells;
//   - a FRESH object with the same property names but a LAYOUT-SHIFTED
//     Structure (extra leading poison property — see makeShiftedGenObject)
//     is fed back to the surviving accessor. A stub or handler-chain node
//     that was not retired when its Structure died crashes/reads poison on
//     traversal of the swept cell; one that matches a RECYCLED StructureID
//     and reads the dead shape's offset hits the shifted layout's poison
//     neighbor instead of the generation's exact expected value (a
//     same-layout rebuild would have returned the correct value through the
//     stale stub, passing vacuously).
// Churners are intentionally still running: cross-thread allocator reuse
// overlaps the structure deaths.
const DEAD_GENS = 8;
const DEAD_WAVE = 64;
const DEAD_WARM = 2000;

function makeGenObject(gen) {
    const o = {};
    o["g" + gen + "_a"] = POISON;
    o["g" + gen + "_f"] = VALUE_A + 100 + gen; // per-generation exact value
    o["g" + gen + "_b"] = POISON;
    return o;
}

// LAYOUT-SHIFTED rebuild for the post-death probes: same g{gen}_f name and
// exact value, but an extra LEADING poison property so g{gen}_f lands at a
// DIFFERENT offset than in the dead Structure (dead: _a@0, _f@1, _b@2;
// shifted: _pre@0, _a@1, _f@2, _b@3). This is what gives the recycled-
// StructureID arm teeth: a correctly-retired stub misses (different
// structure) and the slow path returns the expected value; a stale stub
// matching a RECYCLED StructureID and reading the dead shape's offset (1)
// now reads the poison neighbor instead of accidentally landing on the
// correct slot — a same-layout rebuild would return the right value through
// the stale stub and pass vacuously.
function makeShiftedGenObject(gen) {
    const o = {};
    o["g" + gen + "_pre"] = POISON; // leading slot: shifts every later offset
    o["g" + gen + "_a"] = POISON;
    o["g" + gen + "_f"] = VALUE_A + 100 + gen;
    o["g" + gen + "_b"] = POISON;
    return o;
}

const deadAccessors = [];
for (let gen = 0; gen < DEAD_GENS; ++gen) {
    const expected = VALUE_A + 100 + gen;
    // new Function (not a closure factory) is deliberate: each generation
    // needs its OWN FunctionExecutable/CodeBlock so it warms a fresh
    // get_by_id IC site on that generation's Structure — closures from one
    // factory would share a single IC site, and `o[key]` would be get_by_val,
    // the wrong IC family. The interpolated text is a test-local loop index
    // (no external input), passed through JSON.stringify.
    const getGen = new Function("o", "return o[" + JSON.stringify("g" + gen + "_f") + "];");
    noInline(getGen);

    // Warm the accessor's IC on this generation's (soon-to-die) Structure.
    let wave = [];
    for (let i = 0; i < DEAD_WAVE; ++i)
        wave.push(makeGenObject(gen));
    for (let i = 0; i < DEAD_WARM; ++i) {
        const v = getGen(wave[i % DEAD_WAVE]);
        if (v !== expected)
            throw new Error("dead-gen " + gen + " warmup read " + describe(v) + ", expected " + expected);
    }

    // Kill the Structure: drop every referencing object, sweep, reoccupy.
    wave = null;
    if (haveDollarVM)
        $vm.gc();
    let canaries = [];
    for (let i = 0; i < DEAD_WAVE; ++i)
        canaries.push(makeCanary());

    // Re-exercise the surviving stub against a rebuilt same-named but
    // LAYOUT-SHIFTED shape (see makeShiftedGenObject: stale offset = poison).
    const v = getGen(makeShiftedGenObject(gen));
    if (v !== expected)
        throw new Error("dead-gen " + gen + " post-death read " + describe(v)
            + ", expected " + expected + " (" + CANARY + "=canary, " + POISON
            + "=neighbor poison, 0xbadbeef0-family=zombie scribble: stale stub/handler"
            + " for the dead Structure)");
    canaries = null;
    deadAccessors.push(getGen); // keep all stubs alive across later deaths
    sleepMs(0);
}

// Final sweep over every retained accessor after ALL structure deaths (late
// recycling of an earlier generation's StructureID is exercised here too).
if (haveDollarVM)
    $vm.gc();
for (let gen = 0; gen < DEAD_GENS; ++gen) {
    const v = deadAccessors[gen](makeShiftedGenObject(gen));
    if (v !== VALUE_A + 100 + gen)
        throw new Error("dead-gen " + gen + " final re-poke read " + describe(v)
            + ", expected " + (VALUE_A + 100 + gen));
}

Atomics.store(gate, "stop", 1);
Atomics.notify(gate, "stop", Infinity);
const churnResults = joinAll(churners);
for (let i = 0; i < churnResults.length; ++i) {
    shouldBeTrue(churnResults[i].waves > 0, "churner " + i + " must have completed waves");
    // The fold ties the canary allocations to an observed value: if sinking
    // ever elided them this goes wrong, and the test's pressure claim with it.
    shouldBe(churnResults[i].canaryFold, churnResults[i].waves * CANARY,
        "churner " + i + " canary fold (reoccupation allocations must be real)");
}

verifySurvivors("final");
print("zombie-uaf-canary: PASS");

// WOULD-FAIL-IF: an engine-held pointer into swept memory is dereferenced
// after free+reuse, in either of the two arms this test covers:
//   1. Generation loop (Structures pinned by survivors): allocator-level
//      reuse bugs — cross-thread double-allocation (a churner's getF(o)
//      immediately after allocation reading main's canary 0xc0de), sweep of
//      a live survivor (verifySurvivors reads canary/scribble), or a torn
//      {structureID, offset} IC publish between shapes A/B (reads the
//      neighbor poison 0xdead). Values outside {31, 47} name the
//      canary/poison/scribble seen.
//   2. Structure-death phase: the stub-outlives-structure ic-publish family
//      proper — each dead-generation's Structure actually DIES (every
//      referencing object dropped, swept, cells reoccupied by canaries)
//      while its warmed per-generation accessor survives; re-invoking that
//      accessor on a rebuilt same-named, LAYOUT-SHIFTED shape exercises any
//      IC stub or handler-chain node not retired at structure death. The
//      not-recycled case crashes or reads canary/scribble traversing the
//      swept Structure cell; the RECYCLED-StructureID case reads the dead
//      shape's offset, which the shifted layout fills with neighbor poison
//      0xdead instead of the generation's exact expected value (131+gen) —
//      the shift is load-bearing, since a same-layout rebuild would satisfy
//      the stale stub with the correct value. The final re-poke pass repeats
//      this for late recycling of earlier generations' IDs.
// Under gc-stress-matrix.sh scribble/zombie modes the sweep poisons freed
// cells first, so even a reuse-timing near-miss in either arm trips
// deterministically (or crashes at the exact stale dereference).
