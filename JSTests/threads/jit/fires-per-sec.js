//@ skip (bench-only: needs reportBench from ../bench/harness.js; driven by jit/bench-gates.sh, not the corpus runner)
// fires-per-sec.js — SPEC-jit Task 13: Class-A watchpoint fire throughput,
// RECORDED (never gated). Run via the bench harness:
//   jsc [--useJSThreads=0|1] --useDollarVM=1 ../bench/harness.js fires-per-sec.js
//
// Each measured unit performs FIRES_PER_ITERATION Class-A fires: a fresh
// prototype chain is cached by a fresh hot get function (installing an
// adaptive property watchpoint on the prototype's replacement set), then the
// prototype property is overwritten — firing the set and invalidating the
// dependent code. Flag-on, every one of these fires runs the §5.6 Class-A
// protocol (pre-integration: the Task-1 stub's inline path; post-M4: a real
// STW). Comparing the flag-off and flag-on BENCH lines gives the per-fire
// protocol overhead; bench-gates.sh records both.
//
// Deterministic: fixed counts, fresh functions via the Function constructor
// (same source text each time), no wall-clock dependence in the workload.

const FIRES_PER_ITERATION = 50;
const WARM_CALLS = 200; // enough for Baseline + IC installation; DFG optional

function oneFireCycle(cycleIndex) {
    const proto = { f: 1 };
    const o = Object.create(proto);
    // Fresh function each cycle so its IC starts clean and watches the
    // fresh prototype's replacement set.
    const get = Function("o", "return o.f;");
    let sum = 0;
    for (let i = 0; i < WARM_CALLS; ++i)
        sum += get(o);
    // FIRE: replacing the cached prototype property invalidates the
    // replacement watchpoint set (Class A: dependent IC/code invalidation).
    proto.f = 2;
    sum += get(o); // must observe the new value through the fired path
    if (sum !== WARM_CALLS + 2)
        throw new Error("fire cycle " + cycleIndex + " bad sum: " + sum);
    return sum;
}

reportBench("class-a-fires-x" + FIRES_PER_ITERATION, function () {
    let total = 0;
    for (let i = 0; i < FIRES_PER_ITERATION; ++i)
        total += oneFireCycle(i);
    return total;
}, (WARM_CALLS + 2) * FIRES_PER_ITERATION, 3, 10);
