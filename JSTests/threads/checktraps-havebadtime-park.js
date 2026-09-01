//@ requireOptions("--useJSThreads=1")
// checktraps-dejank-invalidation-point: haveABadTime during a poll-park
// stress.
//
// GIL-off, DFG/FTL CheckTraps no longer clobbers the abstract heap
// (DFGClobberize.h models it as an invalidation point), so butterfly /
// structure facts hoisted across the per-iteration poll stay live at compile
// time. This test exercises the runtime enforcement: while N workers run a
// hot fast-indexed loop (tiered up, butterfly load hoistable across the
// poll), the main thread triggers JSGlobalObject::haveABadTime — the §A.3
// conductor window converts every fast-indexing butterfly to
// (SlowPut)ArrayStorage while the workers sit parked at their polls. The
// IN-WINDOW pre-resume epoch bump (the wrapped-work closure in
// stopTheWorldAndRun's gilOff reroute; the haveABadTimeImpl explicit bump is
// the GIL-on leg) + VMTraps::handleTraps' epoch check must
// jettison each parked worker's on-stack DFG/FTL code, firing the CheckTraps
// invalidation points, so resumed workers OSR-exit at the poll instead of
// reusing the pre-conversion butterfly shape (the +2-slot ArrayStorage vector
// offset corruption signature: silently WRONG element values, no crash).
//
// The test is also an implicit poll-survival check: if CSE ever deleted the
// in-loop poll (the clobberize ordering bug this change documents), the
// haveABadTime stop window could never quiesce the workers and the STW
// watchdog would crash this test at its 30s timeout.
//
// Exactness is the assertion: every hotSum(a, 8000) over a = [1..8] must be
// 36000 before, during, and after the bad-time flip.
load("./resources/assert.js", "caller relative");

const control = new Int32Array(new SharedArrayBuffer(8)); // [0] = stop flag

function hotSum(a, spins) {
    let s = 0;
    for (let i = 0; i < spins; ++i)
        s += a[i & 7];
    return s;
}
noInline(hotSum);

const PER_CALL = 1000 * (1 + 2 + 3 + 4 + 5 + 6 + 7 + 8); // 8000 spins, i&7 uniform => 36000

const workers = spawnN(3, () => {
    const a = [1, 2, 3, 4, 5, 6, 7, 8];
    let calls = 0;
    let total = 0;
    // Atomics.load (a real call) keeps the loop-exit read poll-fresh by
    // construction; the test must not depend on plain-field spin visibility.
    while (!Atomics.load(control, 0)) {
        total += hotSum(a, 8000);
        ++calls;
    }
    return { calls, total };
});

// Tier up the main thread's copy and give the workers time to reach DFG/FTL.
const mine = [1, 2, 3, 4, 5, 6, 7, 8];
for (let i = 0; i < 2000; ++i)
    shouldBe(hotSum(mine, 8000), PER_CALL);

// The flip: an indexed accessor on Object.prototype forces haveABadTime for
// this global — the conductor window rewrites every live fast-indexing
// butterfly, including the workers' arrays, while they are parked at polls.
Object.defineProperty(Object.prototype, 100, {
    get() { return 0xbad; },
    configurable: true,
});

// Keep everyone running well past the conversion so post-resume iterations
// (the dangerous ones: hoisted facts + converted butterflies) execute hot.
for (let i = 0; i < 500; ++i)
    shouldBe(hotSum(mine, 8000), PER_CALL);

Atomics.store(control, 0, 1);
const results = joinAll(workers);

for (const r of results) {
    if (r.calls < 1)
        throw new Error("worker made no progress");
    shouldBe(r.total, r.calls * PER_CALL);
}

// Post-bad-time sanity on the main thread too.
shouldBe(hotSum(mine, 8000), PER_CALL);
delete Object.prototype[100];
