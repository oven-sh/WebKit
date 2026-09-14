//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
//@ threadsRequireGILOff
// The embedder-facing walk of a pending promise's reactions
// (JSPromise::forEachPendingReaction: the module loader's async-cycle check,
// Bun's async stack traces; $vm.pendingPromiseReactionCount here) read the
// promise's packed kind/cell word, its slot and the reaction chain without a
// lock, while GIL off other threads' then() install the first reaction inline
// or spill it and prepend under the promise's cell lock: the walk could pair
// one state's kind with another's cell. It now copies the reactions under the
// cell lock. Every count the main thread takes must be between the counts
// before and after, and never go down while nothing settles the promise.
load("../harness.js", "caller relative");

const THREADS = 4;
const THENS = 400;
const ROUNDS = 20;

for (let round = 0; round < ROUNDS; ++round) {
    let resolve;
    const promise = new Promise(r => { resolve = r; });
    const gate = new Int32Array(new SharedArrayBuffer(8));
    const threads = [];
    for (let t = 0; t < THREADS; ++t) {
        threads.push(new Thread(() => {
            Atomics.add(gate, 0, 1);
            while (Atomics.load(gate, 0) < THREADS) { }
            for (let i = 0; i < THENS; ++i)
                promise.then(() => i);
            Atomics.add(gate, 1, 1);
        }));
    }
    let last = 0;
    while (Atomics.load(gate, 1) < THREADS) {
        const count = $vm.pendingPromiseReactionCount(promise);
        if (count < last || count > THREADS * THENS)
            throw new Error(`round ${round}: reaction count ${count} after ${last}`);
        last = count;
    }
    for (const thread of threads)
        thread.join();
    const final = $vm.pendingPromiseReactionCount(promise);
    if (final !== THREADS * THENS)
        throw new Error(`round ${round}: ${final} reactions, expected ${THREADS * THENS}`);
    resolve(round);
}
