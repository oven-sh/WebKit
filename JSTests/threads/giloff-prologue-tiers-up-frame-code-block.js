//@ requireOptions("--useJSThreads=1", "--useThreadGIL=0", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1")
// Several threads recurse through one function while it tiers up. A caller
// can enter the callee's LLInt prologue through a call link that another
// thread relinks a moment later, when it installs the callee's optimized
// code. The prologue's tier-up check then took the executable's current
// CodeBlock, which was the optimized one, and gave it baseline code. That
// broke every frame that ran it: an assertion in the finalizer, or a crash in
// the optimized code. The prologue now tiers up the CodeBlock of its own frame.
//
// Each round uses a new function, so the tier-up happens once per round. The
// race needs a thread to lose its core in a short window, so the test fails
// most often when other tests run beside it.

const THREADS = 32;
const ROUNDS = 150;
const DEPTH = 300;
const CALLS = 12;

const makers = [];
for (let round = 0; round < ROUNDS; ++round) {
    // The comment makes the source unique, so no round shares a code cache entry.
    makers.push(new Function("n",
        "// round " + round + "\n" +
        "function deep(n) { return n ? deep(n - 1) + 1 : 0; }\n" +
        "return deep(n);"));
}

const gate = { arrived: 0 };
const threads = [];
for (let t = 0; t < THREADS; ++t) {
    threads.push(new Thread(() => {
        for (let round = 0; round < ROUNDS; ++round) {
            // Every thread starts the round together, so the recursions overlap.
            Atomics.add(gate, "arrived", 1);
            while (Atomics.load(gate, "arrived") < (round + 1) * THREADS) { }
            const make = makers[round];
            for (let i = 0; i < CALLS; ++i) {
                const result = make(DEPTH);
                if (result !== DEPTH)
                    throw new Error("thread " + t + ", round " + round + ": deep returned " + result);
            }
        }
        return "done";
    }));
}

for (const thread of threads) {
    if (thread.join() !== "done")
        throw new Error("a thread did not finish");
}
