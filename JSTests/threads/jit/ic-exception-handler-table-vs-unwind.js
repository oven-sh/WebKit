//@ requireOptions("--useJSThreads=1", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=50")
// An optimizing-JIT CodeBlock keeps a table of exception handlers. When an
// inline cache inside a `try` block of such code is regenerated with a case
// that makes a call (a getter), the new stub registers a handler of its own in
// that table, and when the stub dies the entry is removed. With threads,
// another thread can be unwinding an exception through the same CodeBlock at
// that moment and walk the table while it is reallocated. Several threads
// share hot functions of this shape here: half of them keep feeding the caches
// new structures with getters (regenerations), the other half throw from the
// getters (unwinds). Every throw must be caught by the right handler with the
// right value; a crash or a wrong catch is a failure.
load("../harness.js", "caller relative");

const THREADS = 4;
const FUNCTIONS = 24;
const ROUNDS = 300;

function makeHot(seed) {
    return new Function("o", "tag", `
        let r = ${seed};
        try {
            r += o.x;
            try {
                r += o.y;
            } catch (inner) {
                if (inner !== tag + 1)
                    throw new Error("inner caught " + String(inner) + " for tag " + tag);
                r += 1000;
            }
        } catch (outer) {
            if (outer !== tag)
                throw new Error("outer caught " + String(outer) + " for tag " + tag);
            return -1;
        }
        return r;
    `);
}

const hot = [];
for (let i = 0; i < FUNCTIONS; ++i)
    hot.push(makeHot(i));

// Many structures, each with getters for x and y that either return or throw
// what they are told to.
function makeShape(k) {
    const proto = {};
    Object.defineProperty(proto, "x", { get() { if (this.throwX) throw this.tag; return k; }, configurable: true });
    Object.defineProperty(proto, "y", { get() { if (this.throwY) throw this.tag + 1; return 1; }, configurable: true });
    const o = Object.create(proto);
    for (let j = 0; j < (k % 6); ++j)
        o["p" + j] = j; // distinct own structures too
    return o;
}
const SHAPES = 40;
const shapes = [];
for (let k = 0; k < SHAPES; ++k)
    shapes.push(makeShape(k));

// Warm every function on a couple of shapes so they tier up with a small cache.
for (let w = 0; w < 200; ++w) {
    for (let i = 0; i < FUNCTIONS; ++i)
        hot[i](shapes[w & 1], 0);
}

const gate = { go: 0 };
const threads = spawnN(THREADS, (t) => {
    while (Atomics.load(gate, "go") === 0) { }
    let checked = 0;
    for (let round = 0; round < ROUNDS; ++round) {
        for (let i = 0; i < FUNCTIONS; ++i) {
            const f = hot[(i + t) % FUNCTIONS];
            // Feeders walk through all the shapes (cache regenerations);
            // throwers use their own objects and throw from x or from y.
            const k = (t & 1) ? (round * 7 + i) % SHAPES : (round + i) % 3;
            const base = shapes[k];
            const o = Object.create(Object.getPrototypeOf(base));
            const tag = t * 100000 + round * 100 + i;
            o.tag = tag;
            if (!(t & 1)) {
                if (round & 1)
                    o.throwX = true;
                else
                    o.throwY = true;
            }
            const r = f(o, tag);
            if (o.throwX) {
                if (r !== -1)
                    throw new Error("thread " + t + ": throw from x not caught by outer: " + r);
            } else if (o.throwY) {
                if (r !== ((i + t) % FUNCTIONS) + k + 1000)
                    throw new Error("thread " + t + ": throw from y gave " + r);
            } else if (r !== ((i + t) % FUNCTIONS) + k + 1)
                throw new Error("thread " + t + ": no throw gave " + r);
            ++checked;
        }
    }
    return checked;
});
Atomics.store(gate, "go", 1);
for (const checked of joinAll(threads))
    shouldBe(checked, ROUNDS * FUNCTIONS, "calls checked by a thread");
