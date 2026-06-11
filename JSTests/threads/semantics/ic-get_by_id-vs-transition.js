//@ requireOptions("--useJSThreads=1")
// semantics/ic-get_by_id-vs-transition.js — IC matrix: get_by_id driven
// uninit -> mono -> poly -> megamorphic on thread A (here: the main thread,
// tid 0) while thread B concurrently transitions the involved structures
// (filler property adds + delete-driven dictionary flips on the very same
// objects the IC observes).
//
// B never touches the probed property `f` (nor the shape-defining lead
// properties), so every single read has exactly one correct answer. The
// answers are computed independently into `expected`, a main-only side
// table that neither B nor the IC machinery can perturb.
//
// Crucially, f's VALUE is not invariant: A (the sole owner of f) rotates
// every in-window sentinel to a fresh epoch-tagged value every ROTATE_EVERY
// passes, updating `expected` in the same main-thread step. Without the
// rotation, a stale butterfly/offset/IC snapshot would be unobservable here:
// B's flood adds never move f's offset and butterfly growth copies the old
// values, so a read through a pre-transition snapshot would still return the
// correct sentinel. With it, any snapshot that survives a B transition from
// before the latest rotation returns the previous epoch's sentinel and trips
// the compare.
//
// Deterministically green under the phase-1 GIL (cooperative interleaving
// at the Atomics.wait yield points); post-GIL it is a true concurrent
// IC-vs-transition stress, and Tools/threads/amplify.sh widens the windows.
load("../harness.js", "caller relative");

const FAMILIES = 20;          // distinct shapes => f lives at 20 offsets
const OBJS_PER_FAMILY = 4;
const PASSES = 250;

function makeVictim(family, id) {
    const o = {};
    for (let i = 0; i < family; ++i)
        o["lead" + family + "_" + i] = -1; // family-many leading props: f's offset varies per family
    o.f = 1000 + id;
    return o;
}

const victims = [];   // shared with B
const expected = [];  // main-only, never shared
let nextId = 0;
for (let fam = 0; fam < FAMILIES; ++fam) {
    for (let k = 0; k < OBJS_PER_FAMILY; ++k) {
        const id = nextId++;
        victims.push(makeVictim(fam, id));
        expected.push(1000 + id);
    }
}

function getF(o) { return o.f; }
noInline(getF);

// A-owned sentinel rotation (see header). Plain replace-form puts on the
// A-owned slot f: legal under the ownership discipline (B never touches f),
// and expected[] is updated in the same single-threaded main step.
const ROTATE_EVERY = 25;
let epoch = 0;
function rotateSentinels(count) {
    ++epoch;
    for (let i = 0; i < count; ++i) {
        const v = 1000 + i + epoch * 100000;
        victims[i].f = v;
        expected[i] = v;
    }
}

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
        o["flood" + (rounds & 7)] = rounds; // structure transition / butterfly growth
        if ((rounds & 15) === 15) {
            // delete flips the structure into (uncacheable) dictionary mode
            for (let j = 0; j < 8; ++j)
                delete o["flood" + j];
        }
        ++rounds;
        if ((rounds & 63) === 0)
            Atomics.wait(g, "stop", 0, 1); // bounded GIL-dropping yield
    }
    // Report HOW we exited so main can verify B was still mutating when
    // every phase finished (i.e. it left via the stop flag, not the deadline).
    return (Atomics.load(g, "stop") === 1 ? "stopped:" : "deadline:") + rounds;
}, victims, gate);

waitUntil(() => Atomics.load(gate, "bReady") === 1);

// Phase windows over the victim list drive getF's IC through its tiers:
// mono (1 family), poly (3 families), megamorphic (all 20, dictionary-churned).
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
            const v = getF(victims[i]);
            if (v !== expected[i])
                throw new Error(phase.name + " pass " + p + ": victims[" + i + "].f read " + v + ", expected " + expected[i]);
        }
        // Rotate the probed values so stale snapshots become observable:
        // after this, any cached read that predates the rotation (and
        // survived one of B's transitions) returns the old epoch's sentinel.
        if (p % ROTATE_EVERY === ROTATE_EVERY - 1)
            rotateSentinels(phase.count);
        if (p % 50 === 49)
            sleepMs(1); // let B interleave under the cooperative GIL
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

// Quiescent re-check: nothing lost or reordered after the storm.
for (let i = 0; i < victims.length; ++i)
    shouldBe(getF(victims[i]), expected[i], "final victims[" + i + "].f");

// WOULD-FAIL-IF: a get_by_id inline cache (LLInt metadata, Baseline stub, or
// the packed {offset, structureID} word) publishes or consults a stale pair
// while thread B retires the structure underneath it — e.g. the IC observes
// B's post-transition structureID but reads through the pre-transition
// offset (or vice versa), a read goes through a stale butterfly pointer
// from before one of B's growth reallocations, or a dictionary flip leaves
// a cached self-access live. Every victim's f holds a unique sentinel at a
// family-distinct offset AND that sentinel is rotated to a fresh
// epoch-tagged value every ROTATE_EVERY passes by its owning thread, so a
// stale snapshot is observable even though B's churn never moves f's
// offset: it returns the previous epoch's sentinel (or a flood value, -1,
// undefined, or another object's id for torn id/offset pairings and
// cross-structure confusion) and trips the per-read compare against the
// main-only `expected` table on the next read.
