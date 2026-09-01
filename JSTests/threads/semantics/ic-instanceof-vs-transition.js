//@ requireOptions("--useJSThreads=1")
// semantics/ic-instanceof-vs-transition.js — IC matrix: instanceof driven
// uninit -> mono -> poly -> megamorphic on the main thread while thread B
// concurrently transitions BOTH the instances' structures AND the prototype
// objects' structures (adding/deleting filler props on C[k].prototype —
// which churns the very structures the instanceof IC's prototype-chain
// snapshot depends on — without ever changing the chain itself).
//
// The chain is immutable by construction (B only adds/removes own props on
// the prototypes), so `victims[i] instanceof C[k]` has one pinned boolean
// answer per (i, k), computed independently into a main-only table.
//
// Pinned answers alone cannot catch a missed invalidation (a stale cached
// answer equals the pinned answer), so the test adds a main-thread-OWNED
// answer-flipping axis: DETACH extra "detachable" constructors D[s] whose
// .prototype A itself swaps between two candidate prototype objects at
// deterministic pass boundaries while B keeps churning the structures of
// BOTH candidates. The instances' own chains never change; only the
// constructor's .prototype value does, so `dVictims instanceof D[s]` has a
// main-tracked expected boolean that genuinely flips during the run. A
// cached chain snapshot (or cached hit/miss) that survives the swap — i.e.
// an instanceof IC that failed to invalidate while B was concurrently
// replacing the prototype's structure — returns the pre-swap boolean and
// fails the very next assert.
load("../harness.js", "caller relative");

const CTORS = 8;
const OBJS_PER_CTOR = 6;
const PASSES = 150;

const C = [];
for (let k = 0; k < CTORS; ++k) {
    // Distinct constructors with disjoint prototype chains (below Object).
    const ctor = function () { this.tag = k; };
    ctor.prototype = { protoMark: k };
    C.push(ctor);
}

const victims = [];
const ctorOf = []; // main-only
for (let k = 0; k < CTORS; ++k) {
    for (let j = 0; j < OBJS_PER_CTOR; ++j) {
        victims.push(new C[k]());
        ctorOf.push(k);
    }
}

// Detachable constructors: A owns D[s].prototype (B never touches the D
// functions); their instances were created with dProtoA in the chain, so
// `dv instanceof D[s]` is true exactly when D[s].prototype === dProtoA[s].
const DETACH = 2;
const D = [];
const dProtoA = [];
const dProtoB = [];
const dVictims = [];      // DETACH rows of OBJS_PER_CTOR instances
const dOnA = [];          // main-only expected: D[s].prototype is dProtoA[s]
for (let s = 0; s < DETACH; ++s) {
    const ctor = function () { this.dtag = s; };
    dProtoA.push({ dMarkA: s });
    dProtoB.push({ dMarkB: s });
    ctor.prototype = dProtoA[s];
    D.push(ctor);
    const row = [];
    for (let j = 0; j < OBJS_PER_CTOR; ++j)
        row.push(new ctor()); // chain pinned through dProtoA[s] forever
    dVictims.push(row);
    dOnA.push(true);
}

function isInst(o, ctor) { return o instanceof ctor; }
noInline(isInst);

// B churns the structures of every prototype that instanceof can observe:
// all C prototypes plus BOTH candidates of every detachable constructor
// (passed as an explicit list so the churn target does not follow A's
// swaps), and the instance structures of the C victims and D victims alike.
const churnProtos = C.map(function (c) { return c.prototype; }).concat(dProtoA, dProtoB);
let churnObjs = victims.slice();
for (let s = 0; s < DETACH; ++s)
    churnObjs = churnObjs.concat(dVictims[s]);

const gate = { bReady: 0, stop: 0 };
// Must match the literal 60000 inside the mutator entrypoint below (the
// entrypoint cannot capture outer bindings across the thread boundary).
const MUTATOR_DEADLINE_MS = 60000;
const mutatorSpawnedAtMs = Date.now();
const mutator = new Thread((objs, protos, g) => {
    Atomics.add(g, "bReady", 1);
    let rounds = 0;
    // Safety bound is WALL-CLOCK, not a round cap: an unthrottled GIL-off
    // mutator would burn any fixed round budget during (or before) the mono
    // phase and leave the poly/mega phases running with no concurrent
    // transitions at all. The deadline only exists so B terminates if A is
    // wedged; the healthy exit is the stop flag, asserted by main below.
    const DEADLINE_MS = Date.now() + 60000;
    while (Atomics.load(g, "stop") === 0 && Date.now() < DEADLINE_MS) {
        // Churn an instance's structure...
        const o = objs[rounds % objs.length];
        o["flood" + (rounds & 7)] = rounds;
        if ((rounds & 15) === 15) {
            for (let j = 0; j < 8; ++j)
                delete o["flood" + j];
        }
        // ...and a prototype's structure (never any instance's [[Prototype]]
        // link). The explicit list covers all C prototypes and both
        // candidate prototypes of every detachable constructor.
        const proto = protos[rounds % protos.length];
        proto["pflood" + (rounds & 3)] = rounds;
        if ((rounds & 31) === 31) {
            for (let j = 0; j < 4; ++j)
                delete proto["pflood" + j];
        }
        ++rounds;
        if ((rounds & 63) === 0)
            Atomics.wait(g, "stop", 0, 1);
    }
    // Report HOW we exited so main can verify B was still mutating when
    // every phase finished (i.e. it left via the stop flag, not the deadline).
    return (Atomics.load(g, "stop") === 1 ? "stopped:" : "deadline:") + rounds;
}, churnObjs, churnProtos, gate);

waitUntil(() => Atomics.load(gate, "bReady") === 1);

// mono: only C[0] instances vs C[0]. poly: 3 ctors, positive and negative.
// mega: all 8 ctors x both polarities (o instanceof itsCtor, o instanceof
// the next ctor) = 16 cached shapes at the same instanceof site.
const phases = [
    { name: "mono", ctors: 1 },
    { name: "poly", ctors: 3 },
    { name: "mega", ctors: CTORS },
];

// The detachable axis: assert the CURRENT expected boolean for every
// detachable instance through the same noInline instanceof site.
function assertDetachables(phaseName, p, when) {
    for (let s = 0; s < DETACH; ++s) {
        const want = dOnA[s]; // instance chain contains dProtoA[s] only
        for (let j = 0; j < OBJS_PER_CTOR; ++j) {
            const got = isInst(dVictims[s][j], D[s]);
            if (got !== want)
                throw new Error(phaseName + " pass " + p + " (" + when + "): dVictims[" + s + "][" + j + "] instanceof D[" + s + "] was " + got + ", expected " + want + " (stale chain snapshot survived the prototype swap?)");
        }
    }
}

const SWAP_EVERY = 30;
function swapDetachables() {
    for (let s = 0; s < DETACH; ++s) {
        D[s].prototype = dOnA[s] ? dProtoB[s] : dProtoA[s];
        dOnA[s] = !dOnA[s];
    }
}
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
    const count = phase.ctors * OBJS_PER_CTOR;
    for (let p = 0; p < PASSES; ++p) {
        for (let i = 0; i < count; ++i) {
            if (Date.now() - mainPhaseT0 > MAIN_PHASE_BUDGET_MS)
                throw new Error("main phase walk exceeded " + MAIN_PHASE_BUDGET_MS + "ms at phase=" + phase.name + " pass=" + p + " obj=" + i + " — per-op latency collapsed (IC repatch/transition livelock?); the loop is iteration-bounded, so this is engine-side, not test-side");
            const o = victims[i];
            const mine = ctorOf[i];
            const other = (mine + 1) % phase.ctors;
            const pos = isInst(o, C[mine]);
            if (pos !== true)
                throw new Error(phase.name + " pass " + p + ": victims[" + i + "] instanceof C[" + mine + "] was false");
            if (phase.ctors > 1) {
                const neg = isInst(o, C[other]);
                if (neg !== false)
                    throw new Error(phase.name + " pass " + p + ": victims[" + i + "] instanceof C[" + other + "] was true");
            }
            // Sanity: the chain itself is intact (proto walk unchanged).
            if (Object.getPrototypeOf(o) !== C[mine].prototype)
                throw new Error(phase.name + " pass " + p + ": victims[" + i + "] prototype link changed");
        }
        // Detachable axis: every pass re-asserts the current expected
        // boolean; at swap boundaries the answer flips and the FIRST read
        // after the swap must observe the flip — a cached chain snapshot
        // that B's concurrent prototype-structure churn failed to dislodge
        // returns the pre-swap boolean here.
        assertDetachables(phase.name, p, "steady");
        if (p % SWAP_EVERY === SWAP_EVERY - 1) {
            swapDetachables();
            assertDetachables(phase.name, p, "post-swap");
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
    shouldBeTrue(isInst(victims[i], C[ctorOf[i]]), "final positive instanceof for victims[" + i + "]");
    shouldBeFalse(isInst(victims[i], C[(ctorOf[i] + 1) % CTORS]), "final negative instanceof for victims[" + i + "]");
}
// Quiescent detachable re-check against the last swap state.
for (let s = 0; s < DETACH; ++s)
    for (let j = 0; j < OBJS_PER_CTOR; ++j)
        shouldBe(isInst(dVictims[s][j], D[s]), dOnA[s], "final detachable instanceof for dVictims[" + s + "][" + j + "]");

// WOULD-FAIL-IF: the instanceof IC's cached decision (structure-keyed
// prototype-chain snapshot / cached hit-or-miss result, or the
// ctor.prototype watchpoint that guards it) survives an invalidation it
// should have observed while thread B concurrently transitions the
// structures it was keyed on. The detachable axis makes missed invalidation
// directly observable: A swaps D[s].prototype between two candidates whose
// structures B is churning, so a cached pre-swap answer (true or false)
// that the swap + concurrent prototype-structure replacement failed to
// dislodge is returned on the first post-swap read and fails the exact
// boolean compare. The pinned C-victim matrix catches the cross-wiring
// family: a poly/mega stub matching the wrong (structure, ctor) pair after
// B retired a structure flips a pinned positive/negative answer; a
// chain-walk fast path reading a stale prototype slot mid-transition
// crashes or trips the prototype-link sanity check.
