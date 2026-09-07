//@ requireOptions("--useJSThreads=1")
// The baseline/DFG/FTL `create_this` fast path reads a function's allocation
// profile as two words, {allocator, structure}. GIL off another thread can
// clear the profile (a `.prototype` store) or refill it between the two loads;
// with profiled allocation enabled GIL off (fifth round) a torn pair reached
// the inline allocator: a null structure was dereferenced, or an object was
// allocated from a size class that does not match its structure. The fast
// path now reads structure, allocator, structure and takes the slow path
// unless both structure reads agree and are non-null.
load("../harness.js", "caller relative");

function F(a) { this.a = a; this.b = a + 1; this.c = a + 2; }
const protos = [];
for (let i = 0; i < 8; ++i) {
    const p = { which: i };
    // Different inline capacities behind different prototypes make a torn
    // {allocator, structure} pair a size mismatch, not just a null structure.
    for (let k = 0; k < i * 4; ++k) p["pad" + k] = k;
    protos.push(p);
}

const box = { stop: 0 };
const constructors = spawnN(3, () => {
    let n = 0, sum = 0;
    // At least a few batches even if `stop` is already set when this thread
    // first runs (GIL on, the spawned threads get the GIL only when main
    // blocks in joinAll below).
    do {
        for (let i = 0; i < 2000; ++i) {
            const o = new F(i);
            sum += o.c - o.a; // 2
            ++n;
        }
    } while (!Atomics.load(box, "stop") || n < 20000);
    if (sum !== 2 * n) throw new Error("bad objects: " + sum + " vs " + 2 * n);
    return n;
});

// Main: flip F.prototype (clears and refills the allocation profile) while the
// constructors run through the JIT fast path.
const t0 = Date.now();
let flips = 0;
while (Date.now() - t0 < 1500) {
    F.prototype = protos[flips++ & 7];
    for (let i = 0; i < 50; ++i) new F(i); // refill from this thread too
}
Atomics.store(box, "stop", 1);
const total = joinAll(constructors).reduce((a, b) => a + b, 0);
if (total <= 0) throw new Error("constructors did not run");
print("PASS");
