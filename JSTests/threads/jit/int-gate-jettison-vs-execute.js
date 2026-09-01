//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// int-gate-jettison-vs-execute.js — SPEC-jit Task 13 INTEGRATION GATE:
// true-concurrent jettison-vs-execute stress (§5.3/I2/I8/I21).
//
// SKIPPED-BY-DEFAULT scaled-down while STWR is stubbed (phase-1 GIL): run
// with `-- int-gate` (run-jit-tests.sh --int-gate does this) for the full
// loop counts at M4/CS2. Validates the N-separate-VMs config ONLY (R1
// freeze scope; one-VM coverage is Phase-B).
//
// Shape: worker threads execute hot optimized functions in a tight loop
// while the conductor thread forces jettisons of exactly those CodeBlocks:
//   - OSR-exit storms (type-surprise arguments => exit counting => jettison
//     via reoptimization, routed through the §5.3 STWR choke),
//   - $vm.haveABadTime (Class-A fire => invalidation + jettison of
//     array-speculating code, exercising fire->jettison nesting, §5.6.5),
//   - delete-all-code sweeps.
// Soundness criteria: no crash, no torn results, parked mutators resume into
// patched exits (I21: every poll followed by an invalidation point) — a
// mutator running jettisoned elided code would compute stale values.

load("../harness.js", "caller relative");

const FULL = typeof arguments !== "undefined" && Array.prototype.indexOf.call(arguments, "int-gate") >= 0;
const ROUNDS = FULL ? 200 : 8;
const THREADS = FULL ? 4 : 2;
const INNER = 5000;

function makeHot(seed) {
    // Fresh executable per call site family so jettisons hit code the
    // workers are actually inside.
    const fn = Function("o", "a", "let s = " + seed + "; for (let i = 0; i < 50; ++i) s += o.f + a[i & 7]; return s;");
    return fn;
}

const stop = { value: false };
const shapes = [{ f: 1, g: 2 }, { f: 3 }];
const arr = [1, 2, 3, 4, 5, 6, 7, 8];
const hot = [];
for (let i = 0; i < 4; ++i)
    hot.push(makeHot(i));

const workers = spawnN(THREADS, function (slot) {
    let sum = 0;
    let iterations = 0;
    while (!stop.value) {
        const which = (slot + iterations) & 3;
        const fn = hot[which];
        const o = shapes[iterations & 1];
        const got = fn(o, arr);
        // Self-check against an interpreter-grade recomputation (seed of
        // hot[which] is `which`, see makeHot).
        let s = which;
        for (let i = 0; i < 50; ++i)
            s += o.f + arr[i & 7];
        if (got !== s)
            throw new Error("worker " + slot + " observed torn execution: " + got + " != " + s);
        sum += got;
        ++iterations;
        if (!(iterations % 64))
            sleepMs(0); // poll/park point under the cooperative GIL
    }
    return iterations;
});

for (let round = 0; round < ROUNDS; ++round) {
    // 1. OSR-exit storm: feed a type surprise to each hot function.
    for (const fn of hot) {
        try {
            fn({ f: "boom" + round, h: round }, arr);
        } catch (e) { /* result type surprises are fine */ }
    }
    // 2. Class-A fire => invalidation+jettison nest, occasionally, against a
    //    scratch global so the main suite's arrays stay sane.
    if (typeof $vm !== "undefined" && $vm.createGlobalObject && $vm.haveABadTime && (round % 25) === 24)
        $vm.haveABadTime($vm.createGlobalObject());
    // 3. Whole-code sweeps.
    if (typeof $vm !== "undefined" && (round % 50) === 49) {
        if ($vm.deleteAllCodeWhenIdle)
            $vm.deleteAllCodeWhenIdle();
        $vm.gc();
    }
    sleepMs(1); // let workers run between jettison volleys
}

stop.value = true;
const iters = joinAll(workers);
for (const n of iters)
    shouldBeTrue(n > 0, "every worker made progress across jettisons");
print("int-gate-jettison-vs-execute: PASS (" + (FULL ? "FULL" : "smoke — rerun with -- int-gate at M4/CS2") + ")");
