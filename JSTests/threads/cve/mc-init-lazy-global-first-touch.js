//@ requireOptions("--useJSThreads=1")
// MC-INIT surface 7 (docs/threads/cve/map-MC-INIT.md): LazyProperty /
// LazyClassStructure first-touch on a SHARED JSGlobalObject.
//
// Landing gate for SPEC-ungil §K.3 + annex LZ1 (BINDING, unlanded at
// authoring time): the winner of the initializing CAS release-stores the
// result (the release-store IS the publication); foreign threads wait
// park-capably; abandonment resets initializing->empty. Today's in-tree
// LazyPropertyInlines.h:88-106 is the plain-word pre-threads shape: a
// second concurrent first-toucher trips the initializingTag
// RELEASE_ASSERT (crash) or observes an unordered publication.
//
// N threads rendezvous, then simultaneously first-touch a battery of
// lazily-materialized globals (error-subclass structures =
// LazyClassStructure; Intl classes = LazyProperty<.., Structure>). Each
// battery entry is a FIRST touch process-wide (nothing here touches them
// before the gate opens). Detector: every thread gets a working result,
// and all threads agree on the materialized identity (prototype objects
// are ===) — one winner, no leaked default/null, no crash on the
// "being-initialized" state.
//
// EXECUTE POST-UNGIL ONLY (written mid-bring-up; do not run against the
// phase-1 tree). Deterministic rendezvous; race density is good even
// unamplified because all N threads release at one Atomics.notify-class
// edge. Amplifier: Tools/threads/amplify.sh; also run under TSAN no-JIT.
load("../harness.js", "caller relative");

const N = 8;

// Each entry must construct via a lazily-initialized structure/class and
// return [tag, instance, prototypeIdentity].
const battery = [
    () => { const e = new RangeError("x"); return ["RangeError", e instanceof RangeError, Object.getPrototypeOf(e)]; },
    () => { const e = new SyntaxError("x"); return ["SyntaxError", e instanceof SyntaxError, Object.getPrototypeOf(e)]; },
    () => { const e = new ReferenceError("x"); return ["ReferenceError", e instanceof ReferenceError, Object.getPrototypeOf(e)]; },
    () => { const e = new EvalError("x"); return ["EvalError", e instanceof EvalError, Object.getPrototypeOf(e)]; },
    () => { const e = new URIError("x"); return ["URIError", e instanceof URIError, Object.getPrototypeOf(e)]; },
    () => { const e = new AggregateError([], "x"); return ["AggregateError", e instanceof AggregateError, Object.getPrototypeOf(e)]; },
    () => { const c = new Intl.Collator("en"); return ["Intl.Collator", c.compare("a", "b") === -1, Object.getPrototypeOf(c)]; },
    () => { const p = new Intl.PluralRules("en"); return ["Intl.PluralRules", p.select(1) === "one", Object.getPrototypeOf(p)]; },
    () => { const n = new Intl.NumberFormat("en"); return ["Intl.NumberFormat", n.format(7) === "7", Object.getPrototypeOf(n)]; },
    () => { const l = new Intl.ListFormat("en"); return ["Intl.ListFormat", typeof l.format(["a"]) === "string", Object.getPrototypeOf(l)]; },
];

const gate = { started: 0, go: 0 };
const results = { perThread: [] }; // shared; index per thread

const threads = spawnN(N, (index) => {
    Atomics.add(gate, "started", 1);
    while (Atomics.load(gate, "go") === 0)
        Atomics.wait(gate, "go", 0, 100); // bounded quanta (annex T2)

    const mine = [];
    for (let i = 0; i < battery.length; ++i) {
        // Stagger which entry each thread hits FIRST so every battery entry
        // has multiple simultaneous first-touchers.
        const entry = battery[(i + index) % battery.length];
        const [tag, ok, proto] = entry();
        if (!ok)
            throw new Error("thread " + index + ": lazy materialization of " + tag + " produced a broken instance");
        if (proto === null || proto === undefined)
            throw new Error("thread " + index + ": " + tag + " leaked a default/null prototype");
        mine.push([tag, proto]);
    }
    results.perThread[index] = mine;
    return true;
});

waitUntil(() => Atomics.load(gate, "started") === N);
Atomics.store(gate, "go", 1);
Atomics.notify(gate, "go");

for (const r of joinAll(threads))
    shouldBe(r, true);

// Cross-thread identity: exactly one materialization won per lazy slot.
// Compare against a main-thread touch (post-join, so it observes the winner).
for (let index = 0; index < N; ++index) {
    const mine = results.perThread[index];
    shouldBe(mine.length, battery.length, "thread " + index + " completed the battery");
    for (const [tag, proto] of mine) {
        // Re-derive the canonical prototype on the main thread.
        for (const entry of battery) {
            const [tag2, ok2, canonical] = entry();
            if (tag2 !== tag)
                continue;
            shouldBeTrue(ok2, "main-thread re-touch of " + tag);
            if (proto !== canonical)
                throw new Error("thread " + index + ": " + tag + " prototype identity diverged — two lazy materializations both published (MC-INIT: lost single-winner invariant)");
        }
    }
}
