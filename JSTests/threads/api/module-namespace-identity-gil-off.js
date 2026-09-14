//@ requireOptions("--useJSThreads=1")
//@ threadsRequireGILOff
// A module's namespace object is created on first use. GIL off several threads
// could create it at once (spawned threads reading an `export * as` binding
// through another namespace): the module environment's *namespace* slot and the
// record's field could end up holding different objects, so `m.sK` and the
// main thread's import() disagreed (AUDIT R9-15). One object is published now,
// and every thread sees it.
load("../harness.js", "caller relative");

asyncTestStart(1);

const K = 8;
const THREADS = 4;

import("./resources/ns-identity/ns-m.js").then(async (m) => {
    for (let k = 0; k < K; ++k) {
        const gate = new Int32Array(new SharedArrayBuffer(4));
        const threads = [];
        for (let t = 0; t < THREADS; ++t) {
            threads.push(new Thread(() => {
                Atomics.add(gate, 0, 1);
                while (Atomics.load(gate, 0) < THREADS + 1) { }
                return m["s" + k];
            }));
        }
        Atomics.add(gate, 0, 1);
        while (Atomics.load(gate, 0) < THREADS + 1) { }
        const mine = m["s" + k];
        const seen = threads.map(thread => thread.join());
        const imported = await import(`./resources/ns-identity/ns-x${k}.js`);
        for (const ns of seen) {
            if (ns !== mine)
                throw new Error(`s${k}: a thread saw a different namespace object`);
        }
        if (imported !== mine)
            throw new Error(`s${k}: import() returned a different namespace object`);
        if (m["s" + k] !== mine)
            throw new Error(`s${k}: m.s${k} changed`);
    }
    asyncTestPassed();
}).catch((error) => {
    print("FAIL: " + error);
    throw error;
});
