//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// int-gate-stop-budget.js — SPEC-jit Task 13 INTEGRATION GATE: N-thread
// warmup stop-budget bench (INTEGRATE sign-off; bounded by OM F4 chain-fires
// + §5.6 coalescing).
//
// Scaled-down smoke by default; `-- int-gate` at M4/CS2 for the sign-off
// numbers (N-separate-VMs config, R1 freeze scope).
//
// Shape: N threads cold-start and warm up K distinct function families
// (transition-heavy constructors + property/array access) simultaneously —
// the worst case for stop-the-world traffic, since warmup is when ICs
// publish, structures transition, TTL watchpoints install/fire, and tier-up
// jettisons replace code. We measure wall time per phase and print
// machine-parseable STOPBUDGET lines.
//
// Stop COUNT and total STOPPED-TIME need a C++ counter; until the
// `$vm.jsThreadsStopCount()` / `$vm.jsThreadsStoppedMillis()` hooks land
// (requested in docs/threads/INTEGRATE-jit.md, Task 13 section) the wall
// times below are the recorded proxy: the sign-off compares
//   warmup-N-threads vs N * warmup-1-thread
// — a blowup beyond the recorded ceiling means stops are not coalescing
// (§5.6 coalescing REQUIRED) or the stop fan-out is super-linear.

load("../harness.js", "caller relative");

const FULL = typeof arguments !== "undefined" && Array.prototype.indexOf.call(arguments, "int-gate") >= 0;
const THREADS = FULL ? 6 : 2;
const FAMILIES = FULL ? 12 : 4;
const WARM_ITERATIONS = FULL ? 30000 : 5000;

const nowMs = typeof preciseTime === "function" ? () => preciseTime() * 1000 : Date.now;

function warmupBody(familySeed) {
    // Fresh function family per thread per call: forces cold IC publication
    // and structure transitions on every thread.
    let total = 0;
    for (let fam = 0; fam < FAMILIES; ++fam) {
        const ctor = Function("i", "this.a" + familySeed + " = i; this.b = i + 1; this.c = i + 2; this.d = i + 3; this.e" + fam + " = i + 4;");
        const get = Function("o", "return o.b + o.c;");
        const arr = [];
        for (let i = 0; i < WARM_ITERATIONS; ++i) {
            const o = new ctor(i & 0xff);
            total += get(o);
            arr[i & 31] = i;
            total += arr[(i + 7) & 31] | 0;
        }
    }
    return total;
}

// Phase 1: single-thread warmup baseline (fresh families).
let t0 = nowMs();
const single = warmupBody(0);
const singleMs = nowMs() - t0;
shouldBeTrue(single > 0);
print("STOPBUDGET warmup-1-thread " + singleMs.toFixed(1) + " ms");

// Phase 2: N threads warming up fresh families concurrently.
t0 = nowMs();
const threads = spawnN(THREADS, function (slot) {
    return warmupBody(slot + 1);
});
const results = joinAll(threads);
const nThreadMs = nowMs() - t0;
for (const r of results)
    shouldBeTrue(r > 0);
print("STOPBUDGET warmup-" + THREADS + "-threads " + nThreadMs.toFixed(1) + " ms");
print("STOPBUDGET ratio " + (nThreadMs / (singleMs * THREADS)).toFixed(3)
    + " (vs " + THREADS + "x serial; GIL stub ~= 1/THREADS-serialized, post-M4 record + set ceiling at sign-off)");

// Optional C++ counters, once the requested $vm hooks exist:
if (typeof $vm !== "undefined" && typeof $vm.jsThreadsStopCount === "function") {
    print("STOPBUDGET stop-count " + $vm.jsThreadsStopCount());
    if (typeof $vm.jsThreadsStoppedMillis === "function")
        print("STOPBUDGET stopped-millis " + $vm.jsThreadsStoppedMillis().toFixed(1));
} else {
    print("STOPBUDGET stop-count UNAVAILABLE ($vm hook not landed; wall-time proxy only)");
}
print("int-gate-stop-budget: PASS (" + (FULL ? "FULL" : "smoke — rerun with -- int-gate at M4/CS2") + ")");
