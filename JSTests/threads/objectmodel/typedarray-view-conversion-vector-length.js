//@ requireOptions("--useJSThreads=1")
// A wasteful typed-array view has an IndexingHeader (it holds the ArrayBuffer*)
// but no indexed payload. Its flat->segmented conversion (a foreign-thread
// named-property add) must size the spine from a vectorLength of 0: one header
// fragment, no tail fill, and an aliased range that covers only the real
// butterfly. Reading IndexingHeader::vectorLength instead yields the high half
// of the ArrayBuffer pointer, which on a 64-bit heap turns every converted view
// into tens of KB of bogus fragment pointers (and an aliased range that hides
// fresh fragments from the marker).
load("../harness.js", "caller relative");

const VIEWS = 500;
const views = [];
const buffers = [];
for (let i = 0; i < VIEWS; ++i) {
    const ab = new ArrayBuffer(32);
    const ta = new Int32Array(ab);
    ta[1] = 0x1234 + i;
    views.push(ta);
    buffers.push(ab);
}
gc();
const before = gcHeapSize();

// Foreign TID + hasIndexingHeader: every add below converts its view.
new Thread(() => {
    for (let i = 0; i < VIEWS; ++i)
        views[i].p0 = i;
}).join();

// A second foreign add grows out-of-line capacity on the already-segmented
// views (fresh fragments appended to a replacement spine).
new Thread(() => {
    for (let i = 0; i < VIEWS; ++i) {
        for (let p = 1; p < 6; ++p)
            views[i]["p" + p] = i * 10 + p;
    }
}).join();

gc();
gc();
const growth = gcHeapSize() - before;
// A correctly converted view costs on the order of 100 bytes (spine + one
// fresh fragment + the property adds); the pointer-derived geometry costs
// about 60 KB per view.
shouldBeTrue(growth < 8 * 1024 * VIEWS, "heap growth per converted view: " + (growth / VIEWS) + " bytes");

for (let i = 0; i < VIEWS; ++i) {
    const ta = views[i];
    shouldBe(ta.p0, i, "view " + i + " p0");
    for (let p = 1; p < 6; ++p)
        shouldBe(ta["p" + p], i * 10 + p, "view " + i + " p" + p);
    shouldBe(ta[1], 0x1234 + i, "view " + i + " element");
    shouldBe(ta.buffer, buffers[i], "view " + i + " buffer identity");
    shouldBe(Atomics.load(ta, 1), 0x1234 + i, "view " + i + " Atomics.load");
}
