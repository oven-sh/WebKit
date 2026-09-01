//@ requireOptions("--useJSThreads=1", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1")
//@ runDefault("--can-block-is-false")
// SPEC-ungil G.1: GIL-off a spawned Thread may always park synchronously;
// the embedder's blocking policy (--can-block-is-false here) gates only
// main/embedder threads. The spawned probe waits with a zero timeout, so it
// passes the gate and returns "timed-out" without ever blocking, while the
// same call on the main thread keeps throwing. Non-blocking forms are not
// gated on either thread.
//
// The runner appends --can-block-is-false through the //@ runDefault line
// above; the probe below refuses to pass vacuously without it.
load("../harness.js", "caller relative");

{
    let canBlock = true;
    try {
        Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 1);
    } catch {
        canBlock = false;
    }
    if (canBlock)
        throw new Error("spawned-thread-may-block-gil-off.js requires --can-block-is-false");
}

asyncTestStart(1);

const gateMessage = "Atomics.wait cannot be called from the current thread.";

// Main thread: the embedder policy still gates the block.
shouldThrow(TypeError, () => Atomics.wait({ k: 0 }, "k", 0, 0), gateMessage);
shouldBe(Atomics.wait({ k: 0 }, "k", 1), "not-equal");

const t = new Thread(() => {
    const o = { k: 0 };
    const results = [];
    results.push(Atomics.wait(o, "k", 1)); // not-equal: never reaches the gate
    results.push(Atomics.wait(o, "k", 0, 0)); // equal, zero timeout: gated block, permitted on a spawned thread
    return results.join(",");
});

t.asyncJoin().then(v => {
    shouldBe(v, "not-equal,timed-out");
    asyncTestPassed();
});
