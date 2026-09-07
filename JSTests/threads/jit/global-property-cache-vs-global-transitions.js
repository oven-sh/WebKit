//@ requireOptions("--useJSThreads=1")
// get_from_scope GlobalProperty caching flag-on (SPEC-jit §5.5): the slow
// path publishes the {structureID, operand} pair into the bytecode metadata in
// reader order while other threads run the LLInt/Baseline fast path over the
// same metadata, and while the global object itself keeps transitioning (new
// global properties added, one deleted and re-added) so sites re-cache.
// Every read must return the property's value or take the slow path; a
// mismatched pair would read another global's slot. Before this round the
// metadata was frozen flag-on and every global property read (Math, JSON,
// any non-var global) took the slow path: Math.sqrt in a loop ran 14x slower
// than flag-off.
load("../harness.js", "caller relative");

globalThis.ga = 1; globalThis.gb = 2; globalThis.gc = 3;

function reader(rounds) {
    // Fresh functions per thread so each has its own metadata to fill, plus a
    // shared one (sharedReader) whose metadata all threads race to fill.
    const f = new Function("n", "let s = 0; for (let i = 0; i < n; ++i) { s += ga + gb * 2 + gc * 3 + Math.abs(-1); } return s;");
    let total = 0;
    for (let r = 0; r < rounds; ++r) {
        const v = f(2000);
        if (v !== 2000 * (1 + 4 + 9 + 1)) throw new Error("reader saw " + v);
        total += sharedReader(2000);
    }
    return total;
}
globalThis.sharedReader = function (n) { let s = 0; for (let i = 0; i < n; ++i) s += gc - gb - ga; return s; };
globalThis.reader = reader;

const T = 4, ROUNDS = 150;
const threads = [];
for (let t = 0; t < T; ++t) threads.push(new Thread(() => reader(ROUNDS)));

// Meanwhile the main thread transitions the global object: adds properties
// (structure changes, out-of-line storage growth), and churns one extra
// property through delete/re-add (dictionary transitions). ga/gb/gc keep
// their values throughout, so every reader sum is exact.
for (let k = 0; k < 6000; ++k) {
    globalThis["extra" + (k % 512)] = k;
    if ((k & 7) === 0) { delete globalThis.churn; globalThis.churn = k; }
    if ((k & 63) === 0 && sharedReader(10) !== 0) throw new Error("main reader");
}
let sum = 0;
for (const th of threads) sum += th.join();
if (sum !== 0) throw new Error("shared reader sum " + sum);
if (reader(3) !== 0) throw new Error("post");
print("PASS");
