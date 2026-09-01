//@ requireOptions("--useJSThreads=1", "--useJIT=0")
// MC-VAL susceptibility test (docs/threads/cve/map-MC-VAL.md, surface V1):
// LLInt metadata-cache validator/consumer disagreement under N mutators.
//
// Validator: the C++ get/put slow path validates (structure, offset) and
// publishes it into bytecode metadata (LLIntSlowPaths.cpp:837-846).
// Consumer: the asm fast path on EVERY thread re-reads that cache and loads
// at the cached offset. SPEC-jit §4.3 makes the pair one alignas(8) u64
// (GetByIdMetadata.h:50-78) read in a single load with the id half compared
// against the cell — torn or stale (id, offset) pairs must FAIL the compare,
// never consume a mismatched offset. Multi-word caches (proto-load,
// put_by_id transitions, private names) are disabled flag-on (I13/I18).
//
// This storm manufactures exactly the disagreement: a churn thread keeps
// republishing the cache word with (structure, offset) pairs whose offsets
// differ per shape, while reader threads consume through the SAME bytecode
// get_by_id site. Every property value encodes its own name, so consuming a
// stale/mixed pair returns a value with the wrong suffix — detected
// deterministically. --useJIT=0 pins the consumer to the LLInt tier.
//
// Amplifier-ready (Tools/threads/amplify.sh, TSAN no-JIT target): green
// under the phase-1 GIL; post-ungil the relaxed single-u64 republication is
// the actual surface. Bounded loops; every thread joined.
load("../harness.js", "caller relative");

const READERS = 4;
const SLOTS = 8;
const ITERS = 30000;

// Shared pool: pre-created own props so Atomics.load/store apply (api §4.5).
const pool = {};
for (let s = 0; s < SLOTS; ++s)
    pool["p" + s] = null;
const gate = { started: 0, stop: 0, bad: 0 };

// Shape factory: vary the number/order of leading props so .f lands at a
// different PropertyOffset per shape (inline and out-of-line both covered).
function makeObject(shapeId) {
    const o = {};
    for (let j = 0; j < (shapeId % 12); ++j)
        o["lead" + shapeId + "_" + j] = shapeId + ":lead" + j;
    o.f = shapeId + ":f";
    o.g = shapeId + ":g";
    return o;
}

// spawnN passes only the thread index; pool/gate/SLOTS are captured by the
// shared closure scope.
const readers = spawnN(READERS, () => {
    // ONE bytecode site: this get_by_id's metadata word is the contended
    // cache. (Function body is per-thread, but each reader hammers its own
    // site against the churner's foreign structures — and post-ungil the
    // shared-UnlinkedCodeBlock path makes sites genuinely shared.)
    function readF(o) { return o.f; }
    Atomics.add(gate, "started", 1);
    let checks = 0;
    let passes = 0;
    while (Atomics.load(gate, "stop") === 0) {
        for (let s = 0; s < SLOTS; ++s) {
            const o = Atomics.load(pool, "p" + s);
            if (o === null)
                continue;
            const v = readF(o);
            // Wrong-offset consumption returns some OTHER property's value
            // (":lead*" / ":g" suffix) or garbage; both fail here. A stale
            // but self-consistent miss must have re-dispatched to the slow
            // path and produced the correct ":f" value.
            if (typeof v !== "string" || !v.endsWith(":f"))
                Atomics.add(gate, "bad", 1);
            ++checks;
        }
        // Phase-1 GIL is COOPERATIVE-ONLY (SPEC-api item 9: "Phase-1 GIL
        // preemption cooperative-only (G23/G24; yields = 5.2 blocking
        // primitives only)"): a reader that never blocks never yields, so a
        // pure spin here starves the sibling readers and main forever GIL-on
        // (the original shape hung before `started` could even reach
        // READERS — spec-conformant scheduling, not an engine bug; same
        // TEST-BROKEN repair as mc-val-multislot-clone / map-MC-VAL.md V8).
        // The bounded property-path wait parks with the GIL dropped
        // (harness.js sleepMs rationale); GIL-off it costs ~1ms per 256
        // passes and does not weaken the V1 oracle (the LLInt fast path is
        // still the consumer on every check).
        ++passes;
        if ((passes & 255) === 0)
            Atomics.wait(gate, "stop", 0, 1);
    }
    return checks;
});

// Wait for readers, then churn shapes from the main thread (foreign
// publisher relative to the readers).
waitUntil(() => Atomics.load(gate, "started") === READERS);

let shapeId = 0;
for (let i = 0; i < ITERS; ++i) {
    const slot = i % SLOTS;
    Atomics.store(pool, "p" + slot, makeObject(shapeId++));
    if ((i & 1023) === 0) {
        // Also run the same access shape on this thread so the slow path
        // revalidates and republishes the cache word from a second writer.
        const o = Atomics.load(pool, "p" + ((i + 1) % SLOTS));
        if (o !== null && typeof o.f !== "string")
            Atomics.add(gate, "bad", 1);
    }
}

Atomics.store(gate, "stop", 1);
for (const t of readers)
    shouldBeTrue(t.join() >= 0);
shouldBe(Atomics.load(gate, "bad"), 0);
