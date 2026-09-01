// hostcall-hammer.js — targeted JIT-on GIL-off gate for the Group-3
// host-call/catch/varargs reader-writer discriminator amendments
// (getHostCallReturnValue thunk arm, op_catch / OSR-exit-ramp /
// jumpToExceptionHandler same-VM guards, varargsSetup pinned snapshot).
//
// Hammers, per thread, in one hot loop under forced tier-up:
//  - host-call returns through the JIT tiers (Math.max.apply varargs face,
//    parseInt plain face) -> getHostCallReturnValueThunk gilOff arm,
//  - throws that unwind across a host frame into a JS catch
//    ([].forEach callback throw) -> genericUnwind per-lite publish +
//    baseline op_catch / DFG OSR-exit exception ramp / jumpToExceptionHandler
//    consumption,
//  - eval host path (commonCallDirectEval writer arm),
//  - op_call_varargs slow path (the pinned snapshot + echo RELEASE_ASSERT).
//
// PASS criterion: every thread computes the identical checksum.
//
// Invocation (from /root/WebKit):
//   WebKitBuild/<Build>/bin/jsc <GIL-off flags> \
//     --thresholdForJITAfterWarmUp=10 --thresholdForOptimizeAfterWarmUp=100 \
//     --validateFreeListStructure=1 \
//     Tools/threads/bughunt/varargs-fix/hostcall-hammer.js

const W = typeof globalThis.HAMMER_THREADS === "number" ? globalThis.HAMMER_THREADS : 8;
const ITERS = typeof globalThis.HAMMER_ITERS === "number" ? globalThis.HAMMER_ITERS : 60000;

function hot(id) {
    let sum = 0;
    const args = [1, 2, 3, 4, 5];
    const cb = function () { throw new Error("x" + (sum & 7)); };
    for (let i = 0; i < ITERS; ++i) {
        sum += Math.max.apply(null, args); // varargs slow path + host-call return
        sum += parseInt("42", 10); // plain host-call return
        try {
            [1].forEach(cb); // throw across a host frame -> genericUnwind -> catch here
        } catch (e) {
            sum += e.message.length;
        }
        if (!(i & 255))
            sum += eval("1+" + (i & 7)); // direct-eval host-call return arm
        sum += args[i & 3];
        sum &= 0x7fffffff;
    }
    return sum;
}

// Deterministic expectation, computed single-threaded first (also warms the
// code to the upper tiers before the parallel phase).
const expected = hot(-1);

const threads = [];
for (let id = 1; id < W; ++id)
    threads.push(new Thread(hot, id));
const r0 = hot(0);

let ok = r0 === expected;
for (const t of threads) {
    const r = t.join();
    if (r !== expected)
        ok = false;
}
print(ok ? "PASS" : ("MISMATCH expected=" + expected + " got=" + r0));
