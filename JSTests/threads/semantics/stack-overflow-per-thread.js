//@ requireOptions("--useJSThreads=1")
// semantics/stack-overflow-per-thread.js — N threads recurse to stack
// overflow SIMULTANEOUSLY (real barrier, not join-serialized), each must
// receive its OWN RangeError — a distinct exception object, caught at a
// plausible depth for that thread's own stack — and nobody else may be
// disturbed: every thread recovers, overflows a second time (limit re-armed
// after unwind), then does real recursive work; the main thread overflows
// cleanly after the storm.
//
// This directly counter-tests the AB-17 per-lite soft-stack-limit reroute:
// the limit consulted while thread T recurses must be T's own (per-lite)
// limit, and the RangeError/exception state must be T's own, not the
// carrier's or a sibling's (the ab17b per-lite exception-state family).
// Distinct from vmstate/stack-limits-per-thread.js: there the overflows are
// GIL-hand-off-serialized; here all N threads are inside the deep-recursion
// window at the same time, which is exactly where a shared or stale limit
// (or a shared in-flight exception slot) breaks.
load("../harness.js", "caller relative");

const THREADS = 6;

function overflowOnce(tag) {
    let depth = 0;
    function deep() {
        ++depth;
        deep();
    }
    try {
        deep();
    } catch (e) {
        const isRangeError = (e instanceof RangeError) || (e && e.name === "RangeError");
        if (!isRangeError)
            throw new Error(tag + ": expected RangeError, got " + e);
        return { depth, error: e };
    }
    throw new Error(tag + ": recursion never hit the stack limit");
}

const gate = { ready: 0, go: 0 };
const threads = spawnN(THREADS, t => {
    // Barrier: every thread parks here until all N are spawned, then all
    // enter deep recursion together. (Property-path Atomics.wait drops the
    // GIL, so this rendezvous works under phase 1 too.)
    Atomics.add(gate, "ready", 1);
    while (Atomics.load(gate, "go") === 0)
        Atomics.wait(gate, "go", 0, 2);

    const first = overflowOnce("thread " + t + " first");
    // Immediately again: the unwind must have fully re-armed THIS thread's
    // limit (a half-restored limit overflows absurdly early or crashes).
    const second = overflowOnce("thread " + t + " second");
    if (first.depth <= 100 || second.depth <= 100)
        throw new Error("thread " + t + " overflowed implausibly early: " + first.depth + "/" + second.depth);
    // The two unwinds should land in the same ballpark on the same stack;
    // an order-of-magnitude collapse means the second run consulted a
    // foreign/stale limit.
    if (second.depth * 10 < first.depth)
        throw new Error("thread " + t + " second overflow collapsed: " + first.depth + " -> " + second.depth);

    // Post-recovery: real recursive work at depth still succeeds.
    function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
    const fibOK = fib(15) === 610;

    return { t, d1: first.depth, d2: second.depth, e1: first.error, e2: second.error, fibOK };
});

waitUntil(() => Atomics.load(gate, "ready") === THREADS);
// While everyone is parked at the barrier, burn some main-thread stack so
// main's limit state is "interesting" during the storm.
function mainDepthProbe(n) { return n === 0 ? 0 : 1 + mainDepthProbe(n - 1); }
shouldBe(mainDepthProbe(2000), 2000);

Atomics.store(gate, "go", 1);
Atomics.notify(gate, "go", Infinity);

const results = joinAll(threads);

// Every RangeError must be a distinct object — 2 per thread, none shared
// with any other thread (a shared in-flight exception slot would surface
// the SAME error object, or a sibling's, in two places).
const seen = [];
for (const r of results) {
    shouldBeTrue(r.fibOK, "thread " + r.t + " post-recovery fib");
    for (const e of [r.e1, r.e2]) {
        for (const prior of seen) {
            if (e === prior)
                throw new Error("thread " + r.t + " received a RangeError object shared with another overflow site");
        }
        seen.push(e);
    }
    shouldBeTrue(r.e1 !== r.e2, "thread " + r.t + "'s two overflows produced distinct errors");
}
shouldBe(seen.length, THREADS * 2);

// Main thread after the storm: clean overflow + recovery on ITS limit.
const mainResult = overflowOnce("main after storm");
shouldBeTrue(mainResult.depth > 100, "main overflow depth sane after storm");
function fibMain(n) { return n < 2 ? n : fibMain(n - 1) + fibMain(n - 2); }
shouldBe(fibMain(15), 610);

// WOULD-FAIL-IF: the per-lite soft stack limit reroute (AB-17 §A.2.2) or
// per-lite exception state regresses to shared/carrier state — with all N
// threads simultaneously inside the deep-recursion window, a shared limit
// means at least one thread checks ANOTHER stack's bound, so it either
// blows through its real guard page (crash, not RangeError: the catch never
// runs) or faults absurdly early (depth <= 100 / second-overflow collapse
// checks); a shared in-flight exception slot or scope-chain anchor in the
// carrier stack (the ab17b ExceptionScope stack-use-after-return family)
// surfaces as one thread observing a sibling's RangeError object (the
// pairwise-distinctness scan) or as a crash during simultaneous unwinds.
// join-serialized variants of this test cannot trip these because only one
// thread is ever near its limit at a time; the barrier here is the point.
