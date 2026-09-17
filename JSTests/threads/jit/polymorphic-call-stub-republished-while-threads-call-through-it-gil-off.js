//@ requireOptions("--useJSThreads=1")
// SPEC-jit §5.8, history §58 (tenth round): GIL off a callee's tier-up publishes a
// copy of the caller's polymorphic call stub with that slot upgraded, while other
// threads are dispatching through the displaced stub. This is the concurrent side
// of jit/polymorphic-call-site-keeps-its-history-across-callee-tier-up-gil-off.js:
// the main thread builds forty groups of one caller and four callees (fresh
// executables, so every group has its own four-variant site), and four threads
// walk the groups in the same order, all calling through a group's site while its
// callees tier up - Baseline, DFG, FTL, each a republication of that site's stub.
// Every call's result is checked; the test passes by finishing.
load("../harness.js", "caller relative");

const THREADS = 4;
const GROUPS = 40;
const CALLS = 12000;

const groups = [];
for (let g = 0; g < GROUPS; ++g) {
    const caller = new Function("f", "x", "return f(x) + " + g + ";");
    noInline(caller);
    const callees = [];
    for (let k = 0; k < 4; ++k) {
        const tag = g * 4 + k;
        callees.push(new Function("x", "let s = x; for (let i = 0; i < 8; ++i) s = (s + " + tag + ") | 0; return s;"));
    }
    groups.push({ caller, callees, g });
}

const gate = { go: 0 };

function worker(id) {
    while (!Atomics.load(gate, "go")) { }
    let checked = 0;
    for (const { caller, callees, g } of groups) {
        for (let i = 0; i < CALLS; ++i) {
            const k = (i >> 5) & 3; // stretches of 32 calls per callee
            const got = caller(callees[k], i);
            if (got !== (((i + 8 * (g * 4 + k)) | 0) + g))
                throw new Error("thread " + id + " group " + g + " i " + i + ": " + got);
            ++checked;
        }
    }
    return checked;
}

const threads = [];
for (let id = 1; id < THREADS; ++id)
    threads.push(new Thread(worker, id));
Atomics.store(gate, "go", 1);
let total = worker(0);
for (const thread of threads)
    total += thread.join();
shouldBe(total, THREADS * GROUPS * CALLS);
