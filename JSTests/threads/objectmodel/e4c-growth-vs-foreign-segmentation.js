//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
//@ threadsRequireGILOff
// SPEC-objectmodel E4-C (r17): once a structure's thread-local sets are dead,
// the OWNER of an instance grows its out-of-line storage lock-free and
// claim-first - and a foreign thread may convert that very instance to the
// segmented regime (cell lock, claim-first) at any moment, including between
// the owner's word load and its allocation. The growth must therefore copy
// from the word the owner loaded and let the claim's word re-check discard it,
// never re-load the word and assert it flat (sixth round: RELEASE_ASSERT in
// allocateMoreOutOfLineStorage, found by the mirror harness on
// stress/regress-187060.js). Owner: fresh arrays, first out-of-line install
// through the inline cache's reallocating handler; foreign thread: adds a
// property to the same arrays. Every add of both threads must land.
load("../harness.js", "caller relative");

const ROUNDS = 20000;
const box = { slot: null, stop: 0, done: 0 };
// Kill the sets of the empty-array shape and its successors first, so the owner
// is on E4-C from the start.
{ const warm = []; new Thread(() => { warm.zz = 1; }).join(); }

const foreign = new Thread(() => {
    let added = 0;
    while (!Atomics.load(box, "stop")) {
        const a = box.slot;
        if (a && a.f === undefined) { a.f = 7; ++added; }
    }
    return added;
});

const mine = [];
for (let i = 0; i < ROUNDS; ++i) {
    const a = [];
    box.slot = a;            // let the foreign thread at it
    a.constructor = i;       // first out-of-line install (reallocating transition) by the owner
    a.g = i + 1;             // second out-of-line add
    mine.push(a);
}
Atomics.store(box, "stop", 1);
const added = foreign.join();
for (let i = 0; i < ROUNDS; ++i) {
    const a = mine[i];
    if (a.constructor !== i || a.g !== i + 1) throw new Error("owner add lost at " + i + ": " + a.constructor + ", " + a.g);
    if ("f" in a && a.f !== 7) throw new Error("foreign add torn at " + i);
}
if (added < 0) throw new Error("unreachable");
print("PASS");
