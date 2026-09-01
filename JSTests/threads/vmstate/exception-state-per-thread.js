//@ requireOptions("--useJSThreads=1")
// SPEC-vmstate W3 Group 2 (I15): exception/unwind state belongs to the
// JSLock holder. Each thread throws and catches its own marker objects;
// a caught exception must always be the one THIS thread threw (identity,
// payload, and nesting depth), never another thread's — under the GIL the
// VM members m_exception/m_lastException/callFrameForCatch are handed off
// with the lock, and the M6 I15 debug asserts back this up.
load("../resources/assert.js", "caller relative");

const THREADS = 4;
const ROUNDS = 200;

const threads = spawnN(THREADS, t => {
    for (let i = 0; i < ROUNDS; ++i) {
        // Plain throw/catch identity.
        const marker = { tid: t, i, kind: "plain" };
        try {
            throw marker;
        } catch (e) {
            if (e !== marker || e.tid !== t || e.i !== i)
                throw new Error("foreign exception observed: tid " + e.tid + " on thread " + t);
        }

        // Nested throw/rethrow through finally: the rethrown exception must
        // survive the inner handler's unwind state intact.
        const inner = new RangeError("inner " + t + ":" + i);
        const outer = new TypeError("outer " + t + ":" + i);
        let trace = "";
        try {
            try {
                try {
                    throw inner;
                } finally {
                    trace += "f1";
                }
            } catch (e) {
                if (e !== inner)
                    throw new Error("inner identity lost on thread " + t);
                trace += "c1";
                throw outer;
            } finally {
                trace += "f2";
            }
        } catch (e) {
            if (e !== outer)
                throw new Error("outer identity lost on thread " + t);
            trace += "c2";
        }
        if (trace !== "f1c1f2c2")
            throw new Error("unwind order broke on thread " + t + ": " + trace);
    }
    return t;
});

shouldBe(joinAll(threads).join(","), "0,1,2,3");

// Cross-join propagation still works after all that churn (api stub
// contract): a thread that dies with an exception rethrows it at join.
const boom = new Error("vmstate-boom");
const failing = new Thread(() => { throw boom; });
shouldBe(shouldThrow(() => failing.join()), boom);
