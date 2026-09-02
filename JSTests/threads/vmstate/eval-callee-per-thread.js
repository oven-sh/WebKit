//@ requireOptions("--useJSThreads=1")
// Threads run eval on one global object at the same time. With the GIL off,
// this failed in two ways:
//
// - Eval code reads its scope from its callee when it starts, and the
//   interpreter used one callee per global object: it set the callee's scope
//   before an eval and cleared it after. A thread that finished its eval
//   cleared the scope that another thread's eval was about to read. That eval
//   then made functions with no scope, and calling one crashed.
// - A direct eval parsed its code without the compilation lock, and the parser
//   adds to a table on the VM (VM::addSourceProviderCache). Debug builds
//   asserted in that table.
load("../harness.js", "caller relative");

const THREADS = 8;
const EVALS = 300;

const results = joinAll(spawnN(THREADS, (t) => {
    let sum = 0;
    for (let i = 0; i < EVALS; ++i) {
        const local = t;
        const direct = eval("(function (o) { return o.k + local; })");
        const indirect = (0, eval)("(function (o) { return o.k; })");
        sum += direct({ k: i }) - i + indirect({ k: t });
    }
    return sum;
}));

for (let t = 0; t < THREADS; ++t)
    shouldBe(results[t], 2 * t * EVALS, "sum of thread " + t);
