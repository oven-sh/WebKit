//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
//@ threadsRequireGILOff
// SPEC-heap I12, history §37 (tenth round). With two or more clients attached,
// the window-liveness constraint rooted and traced everything a PARKED client
// had allocated since the last marking, so a collection conducted by another
// thread reclaimed none of that thread's young garbage, and a program with a
// busy thread and an idle one grew until a Full collection (JetStream with one
// idle extra thread: 2.4-2.8 GB resident against 1.2-1.35 GB). The constraint
// is off since the tenth round. Here a second thread makes 2,000 young objects
// reachable only through WeakRefs, drops them, and parks; the main thread then
// conducts one Eden collection. Nearly all of them must be gone (the
// conservative scan of the parked thread may keep a few). With the constraint
// on (--useSharedGCWindowLivenessRetention=1) 1,506 are, every run: the ones in
// the parked thread's current blocks stay, and what they reach with them.
load("../harness.js", "caller relative");

const COUNT = 2000;
const state = new Int32Array(new SharedArrayBuffer(8)); // [0]: 1 = the worker has parked its garbage, 2 = it may exit
const shared = { refs: null };

const worker = new Thread(() => {
    function makeGarbage() {
        const refs = [];
        for (let i = 0; i < COUNT; ++i)
            refs.push(new WeakRef({ i, pad: [i, i + 1, i + 2] }));
        return refs;
    }
    noInline(makeGarbage);
    shared.refs = makeGarbage();
    // Overwrite whatever the allocation loop left in this thread's registers and stack.
    (function scrub(depth) { let a = depth, b = depth + 1, c = depth + 2; if (depth) scrub(depth - 1); return a + b + c; })(64);
    releaseWeakRefs(); // the objects a WeakRef's construction keeps alive until the end of its job
    Atomics.store(state, 0, 1);
    Atomics.notify(state, 0);
    while (Atomics.load(state, 0) !== 2)
        Atomics.wait(state, 0, 1);
    return 1;
});

while (Atomics.load(state, 0) !== 1)
    sleepMs(1);
sleepMs(20); // the worker is inside Atomics.wait by now
edenGC();

let dead = 0;
for (const ref of shared.refs) {
    if (ref.deref() === undefined)
        ++dead;
}
Atomics.store(state, 0, 2);
Atomics.notify(state, 0);
worker.join();
if (dead < COUNT * 0.9)
    throw new Error("an Eden collection reclaimed " + dead + " of " + COUNT + " dead young objects that a parked thread had allocated");
