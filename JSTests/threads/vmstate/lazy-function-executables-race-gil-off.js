//@ requireOptions("--useJSThreads=1")
//@ threadsRequireGILOff
// A CodeBlock creates the FunctionExecutable of an inner function the first
// time a new_func for it runs (useLazyFunctionExecutables). Several threads
// running the same outer function for the first time reach the same null
// entry at once. With the GIL off each used to link its own executable and
// store it, and each decremented the block's count of missing entries: the
// count wrapped or stopped above zero (Debug asserts; a DFG compile of the
// block then planted an exit for the entry), and a thread could read another's
// executable through a plain store. The entry is now published by one
// compare-and-swap; a loser uses the winner's executable, and only the winner
// counts. Every thread must see one FunctionExecutable per declaration: the
// functions a declaration makes on different threads share their executable,
// which toString() and a per-executable property cache both observe.
load("../harness.js", "caller relative");

const THREADS = 6;
const ROUNDS = 40;
const INNER = 24;

function makeOuter(round) {
    let body = "";
    for (let i = 0; i < INNER; ++i)
        body += `function f${i}(x) { return x + ${i} + ${round}; }\n`;
    body += "return [" + Array.from({ length: INNER }, (_, i) => `f${i}`).join(", ") + "];";
    return new Function(body);
}

for (let round = 0; round < ROUNDS; ++round) {
    const outer = makeOuter(round);
    const gate = new Int32Array(new SharedArrayBuffer(4));
    const threads = [];
    for (let t = 0; t < THREADS; ++t) {
        threads.push(new Thread(() => {
            Atomics.add(gate, 0, 1);
            while (Atomics.load(gate, 0) < THREADS) { }
            return outer();
        }));
    }
    const results = threads.map(thread => thread.join());
    for (let i = 0; i < INNER; ++i) {
        const expected = results[0][i].toString();
        for (let t = 0; t < THREADS; ++t) {
            const f = results[t][i];
            if (f.toString() !== expected)
                throw new Error(`round ${round}: f${i} differs between threads`);
            if (f(1) !== 1 + i + round)
                throw new Error(`round ${round}: f${i} computes ${f(1)}`);
        }
    }
}
