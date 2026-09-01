//@ requireOptions("--useJSThreads=1")
// semantics/ic-in_by_id-vs-transition.js — IC matrix: in_by_id ("f" in o)
// driven uninit -> mono -> poly -> megamorphic on the main thread while
// thread B transitions the involved structures (filler adds + dictionary
// flips). Half of the structure families HAVE `f`, half do not, so the IC
// caches both positive and negative answers across 20 shapes; B's churn
// must flip neither. A second probe (`h`) is added/deleted by A itself each
// pass, racing B's transitions on the same structure chain.
//
// B never touches `f` or `h`, so every `in` has one pinned answer in the
// main-only `expectedHasF` table.
load("../harness.js", "caller relative");

const FAMILIES = 20;
const OBJS_PER_FAMILY = 4;
const PASSES = 200;

function makeVictim(family, id, hasF) {
    const o = {};
    for (let i = 0; i < family; ++i)
        o["lead" + family + "_" + i] = -1;
    if (hasF)
        o.f = 1000 + id;
    return o;
}

const victims = [];
const expectedHasF = []; // main-only
let nextId = 0;
for (let fam = 0; fam < FAMILIES; ++fam) {
    const hasF = (fam & 1) === 0; // even families have f, odd do not
    for (let k = 0; k < OBJS_PER_FAMILY; ++k) {
        victims.push(makeVictim(fam, nextId++, hasF));
        expectedHasF.push(hasF);
    }
}

function hasF(o) { return "f" in o; }
noInline(hasF);
function hasH(o) { return "h" in o; }
noInline(hasH);

const gate = { bReady: 0, stop: 0 };
// Must match the literal 60000 inside the mutator entrypoint below (the
// entrypoint cannot capture outer bindings across the thread boundary).
const MUTATOR_DEADLINE_MS = 60000;
const mutatorSpawnedAtMs = Date.now();
const mutator = new Thread((objs, g) => {
    Atomics.add(g, "bReady", 1);
    let rounds = 0;
    // Safety bound is WALL-CLOCK, not a round cap: an unthrottled GIL-off
    // mutator would burn any fixed round budget during (or before) the mono
    // phase and leave the poly/mega phases running with no concurrent
    // transitions at all. The deadline only exists so B terminates if A is
    // wedged; the healthy exit is the stop flag, asserted by main below.
    const DEADLINE_MS = Date.now() + 60000;
    while (Atomics.load(g, "stop") === 0 && Date.now() < DEADLINE_MS) {
        const o = objs[rounds % objs.length];
        o["flood" + (rounds & 7)] = rounds;
        if ((rounds & 15) === 15) {
            for (let j = 0; j < 8; ++j)
                delete o["flood" + j];
        }
        ++rounds;
        if ((rounds & 63) === 0)
            Atomics.wait(g, "stop", 0, 1);
    }
    // Report HOW we exited so main can verify B was still mutating when
    // every phase finished (i.e. it left via the stop flag, not the deadline).
    return (Atomics.load(g, "stop") === 1 ? "stopped:" : "deadline:") + rounds;
}, victims, gate);

waitUntil(() => Atomics.load(gate, "bReady") === 1);

const phases = [
    { name: "mono", count: 1 * OBJS_PER_FAMILY },   // family 0 only: all-positive mono
    { name: "poly", count: 3 * OBJS_PER_FAMILY },   // families 0..2: mixed answers
    { name: "mega", count: FAMILIES * OBJS_PER_FAMILY },
];
// Wall-clock tripwire for the main phase walk: the walk itself is a few
// seconds of iteration-bounded work; if engine-side per-op latency collapses
// (GIL-on smoke 2026-06-07 found a reproducible cliff in the put/delete
// variants on the Debug+ASAN build: PASSES=120 finishes in ~2s, PASSES=135
// runs >10min stuck in addNewPropertyTransition under
// operationPutByIdSloppyOptimize; --useJIT=0 finishes in ~3.5s — IC
// repatch/transition livelock family), fail LOUDLY with progress info
// instead of silently eating the harness timeout. LIMITATION: the check sits
// between JS ops, so it converts the slow-but-progressing flavor only; the
// currently-observed flavor wedges INSIDE one put/delete op (the engine
// never returns to JS), where the harness timeout remains the only backstop
// — see staging INTEGRATE.md smoke notes.
const MAIN_PHASE_BUDGET_MS = 45000;
const mainPhaseT0 = Date.now();
for (const phase of phases) {
    for (let p = 0; p < PASSES; ++p) {
        for (let i = 0; i < phase.count; ++i) {
            if (Date.now() - mainPhaseT0 > MAIN_PHASE_BUDGET_MS)
                throw new Error("main phase walk exceeded " + MAIN_PHASE_BUDGET_MS + "ms at phase=" + phase.name + " pass=" + p + " obj=" + i + " — per-op latency collapsed (IC repatch/transition livelock?); the loop is iteration-bounded, so this is engine-side, not test-side");
            const o = victims[i];
            const got = hasF(o);
            if (got !== expectedHasF[i])
                throw new Error(phase.name + " pass " + p + ": (\"f\" in victims[" + i + "]) was " + got + ", expected " + expectedHasF[i]);
            // A-owned h: absent -> present -> absent within one pass, with
            // in_by_id checks at each step, racing B's transitions.
            if (hasH(o) !== false)
                throw new Error(phase.name + " pass " + p + ": h present before add on victims[" + i + "]");
            o.h = p;
            if (hasH(o) !== true)
                throw new Error(phase.name + " pass " + p + ": h missing after add on victims[" + i + "]");
            delete o.h;
            if (hasH(o) !== false)
                throw new Error(phase.name + " pass " + p + ": h still present after delete on victims[" + i + "]");
        }
        if (p % 50 === 49)
            sleepMs(1);
    }
}

Atomics.store(gate, "stop", 1);
Atomics.notify(gate, "stop", Infinity);
// Upper bound on B's elapsed time at the stop store: mutatorSpawnedAtMs
// predates the mutator's own deadline anchor.
const mainElapsedMs = Date.now() - mutatorSpawnedAtMs;
const mutatorReport = mutator.join();
const [mutatorExit, mutatorRounds] = mutatorReport.split(":");
// Vacuity guard, decoupled from wall-clock luck: demand the stop-flag exit
// only when main demonstrably finished well inside B's safety deadline. On a
// slow rung (TSAN, debug no-JIT, loaded host, amplifier sleeps) where main
// itself ran past half the deadline, a "deadline" exit is not a wedge: B
// still mutated concurrently for the full window, which is the property
// this guard protects.
if (mainElapsedMs < MUTATOR_DEADLINE_MS / 2)
    shouldBe(mutatorExit, "stopped", "mutator exited via the stop flag (still mutating when all phases finished): " + mutatorReport);
else
    shouldBeTrue(mutatorExit === "stopped" || mutatorExit === "deadline", "mutator must exit via stop flag or deadline: " + mutatorReport + " (mainElapsedMs=" + mainElapsedMs + ")");
shouldBeTrue(Number(mutatorRounds) >= 1, "mutator must have run at least one round: " + mutatorReport);

for (let i = 0; i < victims.length; ++i) {
    shouldBe(hasF(victims[i]), expectedHasF[i], "final (\"f\" in victims[" + i + "])");
    shouldBeFalse(hasH(victims[i]), "no leftover h on victims[" + i + "]");
}

// WOULD-FAIL-IF: an in_by_id cache keeps serving a structure-keyed
// presence/absence answer across thread B's transition of that structure —
// a cached negative ("f" absent) surviving into a structure where a
// different property layout aliases f's slot, a cached positive surviving a
// dictionary flip that rehashed the PropertyTable, or A's own add/delete of
// h racing B's flood transitions so the brand-new transition is lost and
// `"h" in o` disagrees with the put/delete that A just performed. Both
// polarities are pinned per object (even families true, odd families false;
// h strictly absent->present->absent within a pass), so any stale or torn
// answer fails the exact-boolean compare immediately.
