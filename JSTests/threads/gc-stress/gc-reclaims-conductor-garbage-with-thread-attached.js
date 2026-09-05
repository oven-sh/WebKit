//@ requireOptions("--useJSThreads=1")
// A gc() on the main thread must reclaim the main thread's own fresh garbage
// while another thread is attached to the heap. The shared collector keeps the
// newest cells of every *parked* thread's active blocks for one cycle (their
// only witness may be state the trace cannot see), but the collecting thread's
// stack and registers are scanned exactly as with one thread, so its own
// blocks need no such retention. Before, they got it too, and a batch of
// garbage made just before a gc() survived that gc() whenever a second thread
// was attached.
load("../harness.js", "caller relative");

// A thread that stays attached, parked in a wait, for the whole test.
const gate = { stop: 0, parked: 0 };
const sleeper = new Thread(() => {
    Atomics.store(gate, "parked", 1);
    while (Atomics.load(gate, "stop") === 0)
        Atomics.wait(gate, "stop", 0, 50);
    return 1;
});
waitUntil(() => Atomics.load(gate, "parked") === 1);

const BATCH = 200;

function makeGarbage(refs, tag) {
    // Objects of one size class, made after a collection, so that they sit in
    // the blocks the main thread is allocating from right now. Only WeakRefs
    // to them survive this call.
    for (let i = 0; i < BATCH; ++i)
        refs.push(new WeakRef({ a: i, b: tag, c: i + 1, d: i + 2 }));
}
noInline(makeGarbage);

function countAlive(refs) {
    let alive = 0;
    for (const ref of refs) {
        if (ref.deref() !== undefined)
            ++alive;
    }
    return alive;
}
noInline(countAlive);

let reclaimedFirstTry = 0;
const ROUNDS = 10;
for (let round = 0; round < ROUNDS; ++round) {
    fullGC();
    const refs = [];
    makeGarbage(refs, round);
    // WeakRef targets made in this turn are kept until the turn ends; release
    // them, collect once, and count.
    releaseWeakRefs();
    fullGC();
    const alive = countAlive(refs);
    releaseWeakRefs();
    if (alive <= BATCH / 2)
        ++reclaimedFirstTry;
}

Atomics.store(gate, "stop", 1);
Atomics.notify(gate, "stop");
shouldBe(sleeper.join(), 1, "sleeper result");

// Conservative scanning can keep a few cells of a batch; it cannot keep half of
// every batch. Before the change no round reclaimed its batch on the first
// collection; after it, every round does.
shouldBeTrue(reclaimedFirstTry >= ROUNDS - 2, "rounds whose garbage the first gc() reclaimed: " + reclaimedFirstTry + " of " + ROUNDS);
