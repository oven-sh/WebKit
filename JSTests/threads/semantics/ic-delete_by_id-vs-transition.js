//@ requireOptions("--useJSThreads=1")
// semantics/ic-delete_by_id-vs-transition.js — IC matrix: delete_by_id
// driven uninit -> mono -> poly -> megamorphic on the main thread while
// thread B transitions the same structures (filler adds + its own dictionary
// flips). A owns property `g` exclusively: each pass adds g, verifies it,
// deletes it through a noInline delete site (the IC under test), verifies
// absence, and checks delete-of-absent returns true. Across 20 structure
// families the delete site sees 20 distinct shapes (and, post-flood,
// dictionary structures), pushing it mono -> poly -> megamorphic/generic.
//
// Delete is the nastiest transition (it can flip structures to dictionary
// and creates offset-reuse hazards), so the per-step asserts are exact.
load("../harness.js", "caller relative");

const FAMILIES = 20;
const OBJS_PER_FAMILY = 4;
const PASSES = 150;

function makeVictim(family, id) {
    const o = {};
    for (let i = 0; i < family; ++i)
        o["lead" + family + "_" + i] = -1;
    o.f = 1000 + id; // bystander property: must survive every delete of g
    return o;
}

const victims = [];
const expectedF = []; // main-only
let nextId = 0;
for (let fam = 0; fam < FAMILIES; ++fam) {
    for (let k = 0; k < OBJS_PER_FAMILY; ++k) {
        const id = nextId++;
        victims.push(makeVictim(fam, id));
        expectedF.push(1000 + id);
    }
}

function delG(o) { return delete o.g; }
noInline(delG);
function readG(o) { return o.g; }
noInline(readG);
function readF(o) { return o.f; }
noInline(readF);

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
                delete o["flood" + j]; // B's own deletes: dictionary churn
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
            const token = (p * 1000 + i) * 11 + 7;
            // Delete-of-absent must report true and change nothing.
            if (delG(o) !== true)
                throw new Error(phase.name + " pass " + p + ": delete of absent g returned false on victims[" + i + "]");
            o.g = token;
            if (readG(o) !== token)
                throw new Error(phase.name + " pass " + p + ": g=" + token + " not visible on victims[" + i + "]");
            // The IC under test: delete-of-present.
            if (delG(o) !== true)
                throw new Error(phase.name + " pass " + p + ": delete of present g returned false on victims[" + i + "]");
            if ("g" in o || readG(o) !== undefined)
                throw new Error(phase.name + " pass " + p + ": g survived its delete on victims[" + i + "]");
            // Bystander f must be untouched by the delete (offset-reuse and
            // shape-rollback hazards show up here).
            if (readF(o) !== expectedF[i])
                throw new Error(phase.name + " pass " + p + ": delete of g corrupted f on victims[" + i + "]: " + readF(o));
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
    shouldBeFalse("g" in victims[i], "no leftover g on victims[" + i + "]");
    shouldBe(readF(victims[i]), expectedF[i], "final victims[" + i + "].f intact");
}

// WOULD-FAIL-IF: a delete_by_id cache (or the slow-path delete transition)
// races thread B's transitions on the same structure — the delete committing
// against a structure B already replaced (g resurfaces: `"g" in o` true
// after a true-returning delete), the delete's structure rollback/dictionary
// flip dropping B's concurrently-added flood property or the bystander f
// (offset reuse after the deleted slot is quarantine-released too early
// shows up as f reading a flood value or undefined), or delete-of-absent
// caching a result keyed to a structure that now HAS g. Every step has an
// exact expected outcome (true/true/absent/f-intact with unique tokens), so
// any of these failure shapes trips the assert on the pass where it raced.
