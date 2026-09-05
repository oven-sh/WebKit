//@ requireOptions("--useJSThreads=1", "--useSharedGCHeap=1", "--useConcurrentSharedGCMarking=1", "--collectContinuously=1")
// With the GIL off, two threads that construct with the same function at the
// same time race to fill its FunctionRareData allocation profile, and the fill
// is serialized on a lock in the rare data. The holder allocates under that
// lock (structures, property tables) and may park there for a stop, and the
// waiters poll and park too, so it is a plain lock. It used to be the rare
// data's cell lock, and the concurrent marker's rule is that no cell lock is
// held across an allocation or a park (Heap::stopIfNecessaryForAllClients and
// GCClient::Heap::acquireHeapAccess assert it in Debug with
// --useConcurrentSharedGCMarking), so a Debug build aborted here as soon as a
// fill met a collection. The threads below construct many fresh functions,
// derived classes and bound functions for the first time together, while the
// collector runs continuously.
load("../harness.js", "caller relative");

const THREADS = 4;
const ROUNDS = 150;

const ctors = [];
for (let i = 0; i < ROUNDS; ++i)
    ctors.push(new Function("a", "this.x = a; this.y" + i + " = a;"));
class Base { constructor() { this.b = 1; } }
const derived = [];
for (let i = 0; i < ROUNDS; ++i)
    derived.push(eval("(class D" + i + " extends Base { constructor() { super(); this.d = " + i + "; } })"));

const gate = { go: 0 };
const threads = spawnN(THREADS, () => {
    while (Atomics.load(gate, "go") === 0) { }
    let made = 0;
    for (let i = 0; i < ROUNDS; ++i) {
        const o = new ctors[i](i);
        if (o.x !== i || o["y" + i] !== i)
            throw new Error("plain constructor " + i);
        const d = new derived[i]();
        if (d.d !== i || d.b !== 1)
            throw new Error("derived class " + i);
        const bound = ctors[i].bind(null, i);
        const b = new bound();
        if (b.x !== i)
            throw new Error("bound function " + i);
        made += 3;
    }
    return made;
});
Atomics.store(gate, "go", 1);
for (const made of joinAll(threads))
    shouldBe(made, 3 * ROUNDS, "objects made per thread");
