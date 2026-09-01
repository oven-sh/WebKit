//@ requireOptions("--useJSThreads=1", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1", "--useConcurrentSharedGCMarking=1")
// A spawned thread's exit flushes its per-client mutator mark stack into the
// server's stack. One barrier store into each of more than a segment's worth
// (~510) of distinct old objects, with no GC in between, leaves a
// multi-segment stack at exit; the flush must walk the segment boundaries
// instead of underflowing the head segment, and the remembered-set entries
// it carries must survive into the next eden collection.
load("../resources/assert.js", "caller relative");

const COUNT = 4096;
const olds = [];
for (let i = 0; i < COUNT; ++i)
    olds.push({ a: null, b: null });
fullGC(); // Tenure olds so every store below takes the barrier slow path.

const stored = new Thread(() => {
    let n = 0;
    for (let i = 0; i < COUNT; ++i) {
        olds[i].a = { i: i };
        n++;
    }
    return n;
}).join();
shouldBe(stored, COUNT);

edenGC();
for (let i = 0; i < COUNT; ++i)
    shouldBe(olds[i].a.i, i);
