//@ requireOptions("--useJSThreads=1")
// API-I16 (GI; amplifier+TSAN when present, G15): shape transitions racing
// reads. Targets the THREAD.md numbered object-model invariants from the
// reader side:
//   - no lost properties: every property published at or below the
//     Atomics-published watermark must be readable with its exact value;
//   - no torn shapes: a reader never observes a property name without its
//     value (undefined/garbage) once published, and never crashes while the
//     writer transitions the shape;
//   - no time-travel: the published watermark and an in-place monotonically
//     increasing slot never appear to decrease; o.f === v reads only
//     written values (THREAD.md:5).
//
// Deterministically green under the phase-1 GIL; Tools/threads/amplify.sh
// (randomized yields) and the TSAN no-JIT target shake the post-GIL object
// model. Annex T2: bounded blocking, every thread joined, no
// preemptive-GIL reliance (readers yield via bounded Atomics.wait).
load("../harness.js", "caller relative");

const PROPS = 400;
const READERS = 4;
const o = { seq: 0, mono: 0 };
const gate = { started: 0, stop: 0 };

const readers = spawnN(READERS, () => {
    Atomics.add(gate, "started", 1);
    let lastSeq = 0;
    let lastMono = 0;
    let checks = 0;
    while (Atomics.load(gate, "stop") === 0) {
        const seq = Atomics.load(o, "seq");
        // no time-travel on the published watermark
        if (seq < lastSeq)
            throw new Error("seq time-travel: read " + seq + " after " + lastSeq);
        lastSeq = seq;
        // no lost properties / no torn shapes: everything at or below the
        // watermark is present and exact
        for (let i = 1; i <= seq; ++i) {
            const v = o["p" + i];
            if (v !== i * 1000)
                throw new Error("lost/torn property p" + i + ": got " + v + " at seq " + seq);
        }
        // no time-travel on an in-place overwritten slot (only written
        // values, monotonically increasing by construction)
        const m = o.mono;
        if (!(Number.isInteger(m) && m >= lastMono && m <= PROPS))
            throw new Error("mono read an unwritten or stale value: " + m + " after " + lastMono);
        lastMono = m;
        checks++;
        // bounded yield so the writer can run under the cooperative GIL
        Atomics.wait(gate, "stop", 0, 1);
    }
    return checks;
});

waitUntil(() => Atomics.load(gate, "started") === READERS);

// Writer (main): transition, overwrite, publish — in that store order.
for (let i = 1; i <= PROPS; ++i) {
    o["p" + i] = i * 1000;      // shape transition (butterfly growth)
    o.mono = i;                 // in-place overwrite, increasing
    Atomics.store(o, "seq", i); // SeqCst publication of the watermark
    if (i % 16 === 0)
        sleepMs(1);             // let readers interleave between batches
}

Atomics.store(gate, "stop", 1);
Atomics.notify(gate, "stop", Infinity);
const counts = joinAll(readers);
for (const c of counts)
    shouldBeTrue(c >= 1, "every reader must have completed at least one scan");

// Final state: nothing lost, watermark exact.
for (let i = 1; i <= PROPS; ++i)
    shouldBe(o["p" + i], i * 1000);
shouldBe(o.seq, PROPS);
shouldBe(o.mono, PROPS);
