//@ requireOptions("--useJSThreads=1")
// semantics/ic-get_by_val-vs-transition.js — IC matrix: get_by_val with
// identifier-shaped string keys held in variables (so the by-val IC, not
// get_by_id, is exercised) driven uninit -> mono -> poly -> megamorphic on
// the main thread, while thread B transitions the same structures (filler
// adds + dictionary flips). Megamorphism comes from BOTH axes: 20 structure
// families AND a rotating key set, which pushes the by-val cache through its
// id-cached and generic tiers.
//
// B never writes any probed key, so each (object, key) read has one correct
// answer computed independently into the main-only `expected` table.
//
// The "f" values are NOT invariant: A (sole owner of key "f") rotates every
// in-window sentinel to a fresh epoch-tagged value every ROTATE_EVERY
// passes, updating `expectedF` in the same main-thread step. Without that,
// a stale (identifier, structure, offset) snapshot would be unobservable:
// B's flood adds never move f's offset and butterfly growth copies old
// values, so a pre-transition snapshot would still read the right value.
// The lead keys and family 0's pinned-absent key stay invariant on purpose —
// they pin the presence/absence axis.
load("../harness.js", "caller relative");

const FAMILIES = 20;
const OBJS_PER_FAMILY = 4;
const PASSES = 200;

// Probed keys: `f` on every object plus one family-specific lead name.
function makeVictim(family, id) {
    const o = {};
    for (let i = 0; i < family; ++i)
        o["lead" + family + "_" + i] = -(family * 100 + i);
    o.f = 1000 + id;
    return o;
}

const victims = [];
const expectedF = [];   // main-only
let nextId = 0;
for (let fam = 0; fam < FAMILIES; ++fam) {
    for (let k = 0; k < OBJS_PER_FAMILY; ++k) {
        const id = nextId++;
        victims.push(makeVictim(fam, id));
        expectedF.push(1000 + id);
    }
}

function getV(o, k) { return o[k]; }
noInline(getV);

// Keys passed through a variable; ("f" + "") defeats any literal folding.
const keyF = ("f" + "");
function leadKey(family) { return "lead" + family + "_0"; }

// A-owned sentinel rotation (see header): replace-form by-val puts on the
// A-owned key "f"; expectedF[] updates in the same single-threaded step.
const ROTATE_EVERY = 25;
let epoch = 0;
function rotateSentinels(count) {
    ++epoch;
    for (let i = 0; i < count; ++i) {
        const v = 1000 + i + epoch * 100000;
        victims[i][keyF] = v;
        expectedF[i] = v;
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
    { name: "mono", families: 1 },
    { name: "poly", families: 3 },
    { name: "mega", families: FAMILIES },
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
    const count = phase.families * OBJS_PER_FAMILY;
    for (let p = 0; p < PASSES; ++p) {
        for (let i = 0; i < count; ++i) {
            if (Date.now() - mainPhaseT0 > MAIN_PHASE_BUDGET_MS)
                throw new Error("main phase walk exceeded " + MAIN_PHASE_BUDGET_MS + "ms at phase=" + phase.name + " pass=" + p + " obj=" + i + " — per-op latency collapsed (IC repatch/transition livelock?); the loop is iteration-bounded, so this is engine-side, not test-side");
            const o = victims[i];
            const v = getV(o, keyF);
            if (v !== expectedF[i])
                throw new Error(phase.name + " pass " + p + ": o[\"f\"] on victims[" + i + "] read " + v + ", expected " + expectedF[i]);
            // Family-specific second key (rotates the by-val identifier),
            // absent in family 0 — both presence and absence are pinned.
            const fam = Math.floor(i / OBJS_PER_FAMILY);
            const lk = leadKey(fam);
            const lv = getV(o, lk);
            const lvExpected = fam === 0 ? undefined : -(fam * 100);
            if (lv !== lvExpected)
                throw new Error(phase.name + " pass " + p + ": o[" + lk + "] on victims[" + i + "] read " + lv + ", expected " + lvExpected);
        }
        // Rotate the probed "f" values so a stale by-val snapshot from
        // before the rotation (surviving one of B's transitions) returns
        // the previous epoch's sentinel and fails the compare.
        if (p % ROTATE_EVERY === ROTATE_EVERY - 1)
            rotateSentinels(count);
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

for (let i = 0; i < victims.length; ++i)
    shouldBe(getV(victims[i], keyF), expectedF[i], "final victims[" + i + "][\"f\"]");

// WOULD-FAIL-IF: the get_by_val IC's cached (identifier, structure, offset)
// triple goes stale or tears while thread B transitions the structure — e.g.
// the by-val id cache keeps serving a pre-rotation snapshot (stale butterfly
// pointer or stale offset word) after B's flood transitions, the
// generic-tier PropertyTable lookup races B's dictionary flip and misses a
// present property (or resurrects a deleted flood slot for an absent key),
// or atom identity for the variable-held key diverges between the IC fast
// path and the slow path. The "f" sentinels are rotated to fresh
// epoch-tagged values every ROTATE_EVERY passes by their owning thread, so
// a stale snapshot is observable despite f's fixed offset: it returns the
// previous epoch's sentinel and fails the compare. The lead keys (and
// family 0's pinned-absent lead key, expected `undefined`) pin the
// presence/absence axis, so lost properties and false-positive presence
// fail on that exact read too.
