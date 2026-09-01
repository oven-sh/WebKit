//@ requireOptions("--useJSThreads=1")
// checktraps-dejank-invalidation-point: invalidation-point semantics of the
// de-janked CheckTraps.
//
// Part 1 — hoisted-fact integrity across repeated conductor windows: workers
// run hot loops whose structure/butterfly facts are hoistable across the
// per-iteration poll (GetByOffset of a monomorphic object, fast indexed
// reads), while the main thread repeatedly opens heap-fact-rewriting stop
// windows (Class-A watchpoint fires via structure transitions on shared
// objects, plus reoptimization-grade churn). Every value the workers compute
// must stay exact: a stale hoisted fact reused after a park reads the wrong
// slot/shape and produces a silently wrong sum.
//
// Part 2 — poll survival: the clobberize modeling for CheckTraps def()s
// InvalidationPointLoc; the write(Watchpoint_fire) ordering in
// DFGClobberize.h is what stops CSE from deleting a later poll in favor of an
// earlier invalidation point. If a poll were ever deleted from the hot loop,
// no stop window could quiesce that worker and the STW watchdog would crash
// this test at 30s — completion of the join IS the assertion.
load("./resources/assert.js", "caller relative");

const control = new Int32Array(new SharedArrayBuffer(16)); // [0] = stop flag

// Monomorphic object whose property reads compile to GetByOffset facts that
// LICM may hoist across the in-loop poll once CheckTraps stops clobbering.
const sharedPoint = { x: 3, y: 4, pad0: 0, pad1: 0 };

function hotDot(p, spins) {
    let s = 0;
    for (let i = 0; i < spins; ++i)
        s += p.x * p.x + p.y * p.y; // 25 per iteration, invariant
    return s;
}
noInline(hotDot);

const SPINS = 5000;
const PER_CALL = 25 * SPINS;

const workers = spawnN(3, () => {
    let calls = 0;
    let total = 0;
    while (!Atomics.load(control, 0)) {
        total += hotDot(sharedPoint, SPINS);
        ++calls;
    }
    return { calls, total };
});

// Tier up on the main thread.
for (let i = 0; i < 2000; ++i)
    shouldBe(hotDot(sharedPoint, SPINS), PER_CALL);

// Repeatedly open conductor windows of the kinds the firing-site audit names:
// Class-A watchpoint fires (structure transitions / property additions and
// deletions on hot shared shapes) interleaved with continued hot execution.
// Each window must either leave the workers' hoisted facts true or jettison
// the code whose invalidation point it crossed — never a wrong value.
for (let round = 0; round < 50; ++round) {
    // Structure churn on objects sharing infrastructure with sharedPoint's
    // shape: transitions, fresh shapes, and watchpoint-fire-inducing
    // redefinitions.
    const churn = { x: 1, y: 2, pad0: 0, pad1: 0 };
    churn["extra" + (round & 7)] = round;
    Object.defineProperty(churn, "x", { value: 1, writable: true, configurable: true });
    delete churn.pad1;

    // Hot execution between windows so post-resume iterations run optimized.
    shouldBe(hotDot(sharedPoint, SPINS), PER_CALL);
}

// Part 3 — ANTI-MASKING scenario (amend round, review blocker note): Part 1's
// Class-A churn can jettison the workers' dependent code through the STOCK
// watchpoint fire, which would mask a broken epoch mechanism (the jettison
// happens for the wrong reason but still saves the value). This part removes
// every stock rescue path:
//   - the hot facts are guarded by DYNAMIC CheckStructure only: the point
//     shape's structure-transition watchpoint set is deliberately fired
//     ("burned") BEFORE any worker compiles, so compiled code cannot register
//     it and a later conductor window fires no watchpoint these code blocks
//     depend on;
//   - the hot loop reads ONLY named properties (forced out-of-line by prop
//     count), so the compiled code registers NO havingABadTime dependency;
//   - the object additionally carries fast INDEXED storage, so the
//     haveABadTime conductor window below reallocates its butterfly — moving
//     the out-of-line named slots — while workers may be parked at polls.
// With ctor-only (publication-time) epoch bumps this scenario fails
// deterministically: workers parked BY the window sample the epoch post-bump,
// resume with no jettison, and reuse the hoisted pre-conversion butterfly —
// silently wrong sums. The in-window pre-resume bump is what saves it.
function makeFatPoint() {
    const p = { p0: 0, p1: 0, p2: 0, p3: 0, p4: 0, p5: 0, p6: 0, p7: 0, p8: 0, p9: 0, x: 3, y: 4 };
    p[0] = 10; p[1] = 11; p[2] = 12; p[3] = 13; // Fast indexed storage: makes the butterfly a conversion target.
    return p;
}
// Burn the terminal shape's transition watchpoint before anything compiles
// against it: one extra transition on a sibling of the same shape fires the
// set once, leaving it unwatched for all code compiled afterwards.
{
    const burn = makeFatPoint();
    burn.burned = 1;
}
const fatPoint = makeFatPoint();

function hotDot2(p, spins) {
    let s = 0;
    for (let i = 0; i < spins; ++i)
        s += p.x * p.x + p.y * p.y; // 25 per iteration; named reads only — no indexed read, no bad-time dependency.
    return s;
}
noInline(hotDot2);

const workers3 = spawnN(3, () => {
    let calls = 0;
    let total = 0;
    while (!Atomics.load(control, 0)) {
        total += hotDot2(fatPoint, SPINS);
        ++calls;
    }
    return { calls, total };
});

// Tier up hotDot2 against the burned shape, then give the workers time to
// compile too.
for (let i = 0; i < 2000; ++i)
    shouldBe(hotDot2(fatPoint, SPINS), PER_CALL);

// The genuinely global rewrite: an indexed accessor on Object.prototype
// forces haveABadTime — the conductor window converts fatPoint's indexed
// storage to (SlowPut)ArrayStorage, reallocating its butterfly (named slots
// move), while part-3 workers may sit parked at their polls with hoisted
// butterfly facts and NO watchpoint rescue available (also covered in depth
// by checktraps-havebadtime-park.js, which exercises indexed reads).
Object.defineProperty(Object.prototype, 200, { get() { return -1; }, configurable: true });
shouldBe(hotDot(sharedPoint, SPINS), PER_CALL);
// Keep running well past the conversion so post-resume iterations execute hot.
for (let i = 0; i < 500; ++i)
    shouldBe(hotDot2(fatPoint, SPINS), PER_CALL);
delete Object.prototype[200];

Atomics.store(control, 0, 1);
const results = joinAll(workers);
const results3 = joinAll(workers3);

for (const r of results) {
    if (r.calls < 1)
        throw new Error("worker made no progress");
    shouldBe(r.total, r.calls * PER_CALL);
}
for (const r of results3) {
    if (r.calls < 1)
        throw new Error("part-3 worker made no progress");
    shouldBe(r.total, r.calls * PER_CALL);
}

// Part 4 — §7.1 INTERIM CONTRACT (amend round 2): poll-bounded visibility of
// PLAIN writes. With the CheckTraps clobber de-janked, nothing but the
// interim value-heap writes at the poll (DFGClobberize.h CheckTraps gilOff
// leg: NamedProperties / IndexedProperties / Butterfly_publicLength /
// Absolute / collection fields) stops LICM from hoisting a loop-invariant
// plain-flag load out of a hot spin loop — which would turn these loops
// into hangs. The spin bodies below contain NO calls and NO allocations
// (calls/allocations clobber on their own and would mask the poll-level
// guarantee), and the control flags are deliberately PLAIN, not Atomics —
// unlike Parts 1-3, this part exists precisely to catch the
// plain-field-hoist regression class the review flagged (the other parts'
// Atomics.load control flags cannot). Termination IS the assertion: a
// hoisted flag read spins forever and the harness/watchdog timeout fails
// the test. If the threads memory-model ruling lands NO (plain spin loops
// are allowed to hang; Atomics required), delete this part together with
// the interim writes in DFGClobberize.h.
const plainBox = { stop: 0, a: 1, b: 2 };
const plainArr = [0, 5, 6];

const spinWorkers = spawnN(2, () => {
    let s = 0;
    // Named-field spin: condition + body are pure plain named reads.
    while (!plainBox.stop)
        s += plainBox.a + plainBox.b;
    // Indexed-element spin: pure plain element reads.
    while (!plainArr[0])
        s += plainArr[1] + plainArr[2];
    return s;
});

// Let the spin loops tier up (OSR into DFG/FTL happens inside the spins
// themselves; we just need to give them wall-clock time while staying hot
// ourselves).
for (let i = 0; i < 1000; ++i)
    shouldBe(hotDot(sharedPoint, SPINS), PER_CALL);

// Plain releases — no fence, no Atomics. The poll-bounded-visibility interim
// contract says every spinner must observe these within bounded iterations.
plainBox.stop = 1;
plainArr[0] = 1;
joinAll(spinWorkers);
