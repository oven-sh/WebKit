//@ requireOptions("--useJSThreads=1")
// MC-CODE S6 (docs/threads/cve/map-MC-CODE.md): deferred Class-A fire
// ordering — GIL-removal precondition 10 (INTEGRATE-jit.md; full caveat at
// the WatchpointSet::fireAllSlow(VM&, DeferredWatchpointFire*) overload,
// bytecode/Watchpoint.cpp). A deferring caller (e.g. Structure transition
// paths, runtime/Structure.cpp:1929 row) COMPLETES its watched-fact mutation
// — publishes a new structureID into objects — BEFORE the scope-exit fire's
// stop lands. Under N mutators, another thread's optimized code that elided
// a check on that set executes against the already-false fact in the window
// between publication and fire: the "deopt racing the executing thread" leg
// of MC-CODE. THREAD.md forbids exactly this.
//
// Shape: reader threads run hot optimized property loads against a shared
// object whose shape the owner churns through DEFERRED-fire transition paths
// (delete => dictionary transitions, seal-like reconfigurations, re-adds
// that shuffle property offsets). Two live properties carry disjoint
// sentinel value sets; elided-check code reading at a STALE offset in the
// publication-before-fire window surfaces the OTHER property's sentinel (or
// garbage / a hole) — values a correct execution can never return.
//
// Oracle: reads of o.alpha yield only ALPHA-set values (or throw nothing);
// reads of o.beta yield only BETA-set values. A cross-sentinel, undefined,
// or torn value = the stale-fact window (precondition-10 hole) observed.
//
// The window is publication-to-stop — narrow and scheduler-dependent — so
// this is an AMPLIFIER-READY race test, not deterministic: bounded loops
// here, the amplifier widens the window (and arm64 weak ordering helps the
// attacker). EXECUTED POST-UNGIL ONLY (single-mutator GIL closes the window
// by construction — that is precisely why the precondition is open).
load("../harness.js", "caller relative");

const READERS = 3;
const ROUNDS = 400;
const ALPHA_BASE = 100000; // o.alpha in [ALPHA_BASE, ALPHA_BASE + ROUNDS]
const BETA_BASE = 900000;  // o.beta  in [BETA_BASE,  BETA_BASE + ROUNDS]

const gate = { ready: 0, stop: 0 };
const box = { o: null };

function freshTarget(round) {
    const o = {};
    o.alpha = ALPHA_BASE + round;
    o.beta = BETA_BASE + round;
    o.gamma = -1; // churn fodder: deleted/re-added to drive transitions
    return o;
}
box.o = freshTarget(0);

const readers = spawnN(READERS, () => {
    Atomics.add(gate, "ready", 1);
    function hotAlpha(o) { return o.alpha; }
    function hotBeta(o) { return o.beta; }
    noInline(hotAlpha);
    noInline(hotBeta);
    let checks = 0;
    while (Atomics.load(gate, "stop") === 0) {
        const o = box.o;
        for (let i = 0; i < 256; ++i) {
            const a = hotAlpha(o);
            // `alpha` exists for the object's whole life; only its offset
            // moves under the owner's transitions. A BETA-range, undefined,
            // or out-of-range value = a read through a stale elided-check
            // body at a wrong offset.
            if (!(typeof a === "number" && a >= ALPHA_BASE && a < BETA_BASE))
                throw new Error("o.alpha read " + String(a) + " — stale-offset read in the publication-before-fire window");
            const b = hotBeta(o);
            if (!(typeof b === "number" && b >= BETA_BASE))
                throw new Error("o.beta read " + String(b) + " — stale-offset read in the publication-before-fire window");
            ++checks;
        }
    }
    return checks;
});

waitUntil(() => Atomics.load(gate, "ready") === READERS);

for (let r = 1; r <= ROUNDS; ++r) {
    const o = box.o;
    // Deferred-fire transition storm on the LIVE object the readers' hot
    // code is specialized against:
    //  - delete drives toward dictionary mode (deferred structure-set fires),
    //  - re-adds shuffle out-of-line offsets,
    //  - value updates stay inside each property's sentinel range.
    delete o.gamma;
    o["g" + (r & 7)] = r;      // fresh keys: out-of-line growth + transitions
    o.gamma = -1;
    delete o["g" + ((r + 4) & 7)];
    o.alpha = ALPHA_BASE + r;
    o.beta = BETA_BASE + r;
    if ((r & 31) === 0)
        box.o = freshTarget(r); // fresh shape lineage: re-warms reader ICs/DFG
}
Atomics.store(gate, "stop", 1);

const counts = joinAll(readers);
for (const c of counts)
    shouldBeTrue(c > 0);
print("mc-code-deferred-fire-stale-window: PASS (" + counts.join(",") + " checks)");
