//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
//@ threadsRequireGILOff
// SPEC-jit §5.6 "Deferred claims in flight", history §55 (GIL-removal
// precondition 10, narrowed). The first transition out of a structure that
// optimized code watches claims the structure's transition set at once and fires
// it (invalidating that code) only at the end of the transition. A second thread
// that makes its own transition out of the same structure in between finds the
// set already claimed, has nothing to fire, publishes, and returns into its own
// optimized code, which nobody has invalidated yet: that code still believes its
// object has the old structure - across the very call that changed it. For two
// kinds of transition the stale code is not merely stale but unsafe, and both
// are tested here, each round with a FRESH structure (a property name no round
// used before) so that the set is watched again:
//  (1) data -> accessor: `r + o.p` after a call that turned o.p into a getter
//      adds a GetterSetter cell (a release assertion in JSCell::toPrimitive);
//  (2) Double -> Contiguous: `a[2] = 2.3023e-320` after a call that stored an
//      object into a[0] writes raw double bits into a boxed lane, and reading
//      the lane back dereferences 0x1234.
// Found by the mirror harness, whose two threads start together
// (stress/create-this-structure-change.js 6 of 40 runs,
// stress/arith-nodes-abstract-interpreter-untypeduse.js 2-5 of 60). Such
// transitions now wait out every other thread's claimed-but-unfired set, claim
// the set themselves if it is still watched, and publish and fire in one stop
// (the fire still comes after the publication, which adaptive watchpoints
// need: objectmodel/indexing-transition-keeps-adaptive-watchpoint.js).
load("../harness.js", "caller relative");

const THREADS = 3;
const ROUNDS = 60;
const WARMUP = 30000;
const small = 2.3023e-320; // 0x1234 as raw bits

// A reusable barrier on shared properties.
const barrier = { count: 0, generation: 0 };
function arrive() {
    const generation = Atomics.load(barrier, "generation");
    if (Atomics.add(barrier, "count", 1) === THREADS - 1) {
        Atomics.store(barrier, "count", 0);
        Atomics.add(barrier, "generation", 1);
        return;
    }
    while (Atomics.load(barrier, "generation") === generation) { }
}

// Per round, shared by all threads (one CodeBlock each, so one set of watchpoints on the round's structures).
const rounds = [];
for (let r = 0; r < ROUNDS; ++r) {
    const p = "p" + r;
    rounds.push({
        makeObject: new Function("return { " + p + ": 43, q: 1 };"),
        readTwice: new Function("o", "between", "var first = o." + p + "; between(o); return first + o." + p + ";"),
        redefine: new Function("o", "o.__defineGetter__('" + p + "', function () { return 666; });"),
        makeArray: new Function("var a = [1.1, 2.2, 3.3]; a." + p + " = 1; return a;"),
        storeAround: new Function("a", "c", "small", "var tag = a." + p + "; a[0] = 1.1; a[1] = 2.2; var t = -c; a[2] = small; return tag;"),
    });
}
const nothing = function (o) { };

function worker(id) {
    for (let r = 0; r < ROUNDS; ++r) {
        const round = rounds[r];

        // (1) Warm readTwice on objects of the round's structure, without transitions.
        for (let i = 0; i < WARMUP; ++i) {
            if (round.readTwice(round.makeObject(), nothing) !== 86)
                throw new Error("round " + r + ": warm-up read");
        }
        // (2) Warm storeAround on Double arrays of the round's structure.
        for (let i = 0; i < WARMUP; ++i)
            round.storeAround(round.makeArray(), "1", small);

        const mine = round.makeObject();
        const myArray = round.makeArray();
        const converting = { toString() { myArray[0] = {}; return "3"; } };

        arrive(); // Everybody is in optimized code that watches the round's structures.
        const sum = round.readTwice(mine, round.redefine);
        if (sum !== 43 + 666)
            throw new Error("round " + r + " thread " + id + ": data then getter gave " + String(sum));

        arrive();
        round.storeAround(myArray, converting, small);
        const back = myArray[2];
        if (back !== small)
            throw new Error("round " + r + " thread " + id + ": the lane reads " + String(back));
        if (typeof myArray[0] !== "object" || myArray[1] !== 2.2)
            throw new Error("round " + r + " thread " + id + ": the array reads " + String(myArray[0]) + ", " + String(myArray[1]));
        arrive();
    }
    return id;
}

const threads = [];
for (let id = 1; id < THREADS; ++id)
    threads.push(new Thread(worker, id));
worker(0);
for (const thread of threads)
    thread.join();
