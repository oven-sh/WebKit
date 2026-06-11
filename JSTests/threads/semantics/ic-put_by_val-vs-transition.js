//@ requireOptions("--useJSThreads=1")
// semantics/ic-put_by_val-vs-transition.js — IC matrix: put_by_val with
// identifier-shaped string keys held in variables, replace form (existing
// `f`) and transition form (fresh A-owned `g`), driven uninit -> mono ->
// poly -> megamorphic on the main thread while thread B transitions the same
// structures (filler adds + dictionary flips).
//
// Ownership discipline: A alone writes keys "f" and "g"; B alone writes
// floodN keys. Every put is immediately read back through an independent
// noInline get, so each store has exactly one legal observable result.
load("../harness.js", "caller relative");

const FAMILIES = 20;
const OBJS_PER_FAMILY = 4;
const PASSES = 120;

function makeVictim(family, id) {
    const o = {};
    for (let i = 0; i < family; ++i)
        o["lead" + family + "_" + i] = -1;
    o.f = 1000 + id;
    return o;
}

const victims = [];
let nextId = 0;
for (let fam = 0; fam < FAMILIES; ++fam)
    for (let k = 0; k < OBJS_PER_FAMILY; ++k)
        victims.push(makeVictim(fam, nextId++));

function putV(o, k, v) { o[k] = v; }
noInline(putV);
function getV(o, k) { return o[k]; }
noInline(getV);

const keyF = ("f" + "");
const keyG = ("g" + "");

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
    { name: "mono", count: 1 * OBJS_PER_FAMILY },
    { name: "poly", count: 3 * OBJS_PER_FAMILY },
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
            const token = (p * 1000 + i) * 13 + 5;
            // Replace-form put_by_val.
            putV(o, keyF, token);
            const back = getV(o, keyF);
            if (back !== token)
                throw new Error(phase.name + " pass " + p + ": o[\"f\"]=" + token + " on victims[" + i + "] read back " + back);
            // Transition-form put_by_val: g absent -> present -> deleted.
            putV(o, keyG, token + 1);
            const gBack = getV(o, keyG);
            if (gBack !== token + 1)
                throw new Error(phase.name + " pass " + p + ": o[\"g\"]=" + (token + 1) + " on victims[" + i + "] read back " + gBack);
            delete o.g;
            if (getV(o, keyG) !== undefined)
                throw new Error(phase.name + " pass " + p + ": deleted g still visible on victims[" + i + "]");
        }
        if (p % 40 === 39)
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
    shouldBeTrue(typeof getV(victims[i], keyF) === "number", "final victims[" + i + "][\"f\"] is a number");
    shouldBeFalse("g" in victims[i], "no leftover g on victims[" + i + "]");
}

// WOULD-FAIL-IF: the put_by_val IC commits a store through a stale cached
// (identifier, structure, offset) triple while thread B transitions the same
// object — the replace-put writing f at a pre-realloc butterfly slot (the
// token is lost; read-back sees the previous value), the transition-put for
// g racing B's flood transition on the SAME structure so one of the two
// added properties vanishes or both land on one offset, or the generic path
// racing a dictionary flip so the delete/read-back of g disagrees. Unique
// per-pass tokens with an immediate independent read-back make any lost,
// misplaced, or doubled store fail the compare on the pass where it tore.
