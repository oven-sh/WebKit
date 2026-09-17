//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--forceEagerCompilation=1")
//@ threadsRequireGILOn
// SPEC-jit §5.7.3, history §49 (tenth round). GIL on, a synchronous compile
// path (--useConcurrentJIT=false, which --forceEagerCompilation and
// --useProfiler imply) is supported: the compilation claims its key for its
// whole duration like a concurrent plan, so a second thread that reaches the
// same tier-up after a handoff defers instead of compiling the same code again.
// Before, the option parser failed fast ("useJSThreads requires
// useConcurrentJIT") in every configuration; GIL off it still does (a
// synchronous compilation there holds heap access with no poll until it ends),
// which is why this test is GIL-on only.
// Two threads run the same functions past every tier's threshold with eager
// thresholds and check every result; then both add properties to objects of
// shared shapes (transitions compiled synchronously) across handoffs.
load("../harness.js", "caller relative");

function sumTo(n) { let s = 0; for (let i = 0; i < n; ++i) s += i; return s; }
function build(k) { const o = { a: k }; o.b = k + 1; o.c = k + 2; o.d = o.a + o.b + o.c; return o; }
function work(rounds) {
    let acc = 0;
    for (let r = 0; r < rounds; ++r) {
        acc += sumTo(100) - 4950; // 0
        const o = build(r);
        if (o.d !== 3 * r + 3)
            throw new Error("bad object " + JSON.stringify(o));
        acc += o.d - (3 * r + 3);
    }
    return acc;
}

const box = { turn: 0 };
const t = new Thread(() => {
    let total = 0;
    for (let i = 0; i < 40; ++i) {
        total += work(200);
        Atomics.store(box, "turn", i); // a store the main thread waits on: a handoff point
    }
    return total;
});
let mine = 0;
for (let i = 0; i < 40; ++i) {
    mine += work(200);
    Atomics.wait(box, "turn", -1, 1); // parks briefly with the GIL dropped
}
const theirs = t.join();
if (mine !== 0 || theirs !== 0)
    throw new Error("sums " + mine + " " + theirs);
if (!numberOfDFGCompiles(sumTo) && !$vm.useJIT())
    ; // JIT-less runs have nothing to compile
else if ($vm.useJIT() && !numberOfDFGCompiles(sumTo))
    throw new Error("sumTo was never compiled");
