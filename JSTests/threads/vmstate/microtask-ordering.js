//@ requireOptions("--useJSThreads=1")
// SPEC-vmstate W3 Group 6 / §6.5 Phase A: VM::queueMicrotask/drainMicrotasks
// are NOT rerouted to per-thread VMLite queues (§6.1.1) — microtask ordering
// must be exactly today's, on the main thread and inside spawned threads.
// (I11 — owner-only enqueue/drain on a per-thread queue — is a C++-level
// debug assert exercised by the VMLiteInlines unit-test helpers; this file
// pins the JS-observable contract those helpers must not disturb.)
load("../resources/assert.js", "caller relative");

function microtaskOrderProbe() {
    const order = [];
    Promise.resolve("a").then(v => {
        order.push(v);
        Promise.resolve("nested").then(v2 => order.push(v2));
    }).then(() => order.push("b"));
    Promise.resolve("c").then(v => order.push(v));
    (async () => {
        order.push("sync-async");
        await null;
        order.push("after-await");
    })();
    order.push("sync");
    drainMicrotasks();
    return order.join(",");
}

const EXPECTED = "sync-async,sync,a,c,after-await,nested,b";

// Main thread.
shouldBe(microtaskOrderProbe(), EXPECTED);

// Spawned threads: same FIFO semantics; each thread drains its enqueued jobs
// to completion before returning (Phase A: one VM queue, GIL-serialized).
const digests = joinAll(spawnN(3, () => microtaskOrderProbe()));
for (let t = 0; t < 3; ++t)
    shouldBe(digests[t], EXPECTED, "thread " + t);

// Interleaved enqueue across threads: a thread's pending microtasks must run
// in that thread's drain, with payload integrity (no cross-thread loss).
const seen = joinAll(spawnN(3, t => {
    const local = [];
    for (let i = 0; i < 50; ++i)
        Promise.resolve(t * 100 + i).then(v => local.push(v));
    drainMicrotasks();
    if (local.length !== 50)
        throw new Error("thread " + t + " lost microtasks: " + local.length);
    for (let i = 0; i < 50; ++i) {
        if (local[i] !== t * 100 + i)
            throw new Error("thread " + t + " microtask order broke at " + i);
    }
    return local.length;
}));
shouldBe(seen.join(","), "50,50,50");

// Main thread again after thread teardown.
shouldBe(microtaskOrderProbe(), EXPECTED);
