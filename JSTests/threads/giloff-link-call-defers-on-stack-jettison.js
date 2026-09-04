//@ requireOptions("--useJSThreads=1", "--useThreadGIL=0", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1")
// GIL-off, a call that links a call site defers traps, because it links to a
// CodeBlock that it read before. Trap handling jettisons the optimized code on
// this thread's stack when another thread's request has moved the heap-fact
// epoch, and that must wait for the deferral too. The link already wrote the
// callee's CodeBlock into the new frame, which is the top of the stack, so
// without the wait the link finds the CodeBlock it read jettisoned.

const ROUNDS = 400;
const CHURN_THREADS = 4;
const POOL = 8;

function target(x) { return x + 1; }
noInline(target);
for (let i = 0; i < 100000; ++i)
    target(i);

const state = { stop: false, pool: [] };
for (let i = 0; i < POOL; ++i)
    state.pool.push({ a: 0, b: 0 });

// Each request for a stop moves the heap-fact epoch. Writes and property adds
// on objects that several threads write make such requests.
const churn = [];
for (let t = 0; t < CHURN_THREADS; ++t) {
    churn.push(new Thread((me) => {
        let count = 0;
        while (!state.stop) {
            for (let i = 0; i < POOL; ++i) {
                const o = state.pool[i];
                o.a = me;
                o.b = count;
                o["w" + me + "_" + (count & 31)] = count;
            }
            if (!(++count & 31))
                state.pool[count & (POOL - 1)] = { a: me, b: count };
        }
        return count;
    }, t));
}

// A new function has new call sites, and a call site links on its second call,
// to target's optimized code. The round number keeps the code cache from
// sharing one CodeBlock between rounds.
const SITES = 100;
const body = "let r = 0; for (let i = 0; i < 2; ++i) {" + " r += f(x);".repeat(SITES) + "} return r;";
// Each stop is slow in a Debug build, so the loop also ends at a deadline.
const deadline = Date.now() + 5000;
for (let round = 0; round < ROUNDS && Date.now() < deadline; ++round) {
    const caller = new Function("f", "x", "// round " + round + "\n" + body);
    const result = caller(target, round);
    if (result !== 2 * SITES * (round + 1))
        throw new Error("bad result " + result + " at round " + round);
}

state.stop = true;
for (const thread of churn)
    thread.join();
