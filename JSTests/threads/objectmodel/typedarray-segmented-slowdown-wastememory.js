//@ requireOptions("--useJSThreads=1")
// A Fast/Oversize typed-array view whose butterfly has been SEGMENTED by a
// foreign-thread named-property add (the view has inline capacity 0, so the
// add is out-of-line and, under the GIL, routes through the segmented
// conversion) must still be able to take the wastage transition: .buffer,
// Atomics validation and the spread/iterator paths reach
// JSArrayBufferView::slowDownAndWasteMemory with a segmented word and no
// header fragment. The transition publishes a replacement spine carrying a
// header fragment; previously it dereferenced butterfly() on the spine and
// tripped RELEASE_ASSERT(!isSegmentedButterfly(expected)).
load("../harness.js", "caller relative");

const ROUNDS = 20;

// (1) Fast view: main adds an out-of-line property, worker adds another
// (segments), main reads .buffer.
for (let i = 0; i < ROUNDS; ++i) {
    const ta = new Int32Array(8);
    ta[2] = 0x1234 + i;
    ta.a = i;
    new Thread(() => { ta.b = i + 1; }).join();
    const buf = ta.buffer;
    shouldBeTrue(buf instanceof ArrayBuffer, "round " + i + ": segmented Fast .buffer");
    shouldBe(buf.byteLength, 32, "round " + i + ": segmented Fast byteLength");
    shouldBe(ta[2], 0x1234 + i, "round " + i + ": element survived segmented wastage");
    shouldBe(ta.a, i, "round " + i + ": main named prop survived");
    shouldBe(ta.b, i + 1, "round " + i + ": worker named prop survived");
    shouldBe(ta.buffer, buf, "round " + i + ": .buffer identity is stable");
    ta.c = 7;
    shouldBe(ta.c, 7, "round " + i + ": post-wastage add on segmented view");
    shouldBe(new Int32Array(buf)[2], 0x1234 + i, "round " + i + ": buffer aliases the view");
}

// (2) Oversize view (large, no explicit buffer): the vector is adopted by the
// new ArrayBuffer after the segmented publication.
for (let i = 0; i < ROUNDS; ++i) {
    const ta = new Float64Array(4096);
    ta[4095] = i + 0.5;
    ta.a = i;
    new Thread(() => { ta.b = i; }).join();
    const buf = ta.buffer;
    shouldBe(buf.byteLength, 4096 * 8, "round " + i + ": segmented Oversize byteLength");
    shouldBe(ta[4095], i + 0.5, "round " + i + ": Oversize element survived");
    shouldBe(new Float64Array(buf)[4095], i + 0.5, "round " + i + ": Oversize buffer aliases the view");
    shouldBe(ta.a, i, "round " + i + ": Oversize main named prop survived");
    shouldBe(ta.b, i, "round " + i + ": Oversize worker named prop survived");
}

// (3) The worker takes the wastage transition on the segmented view it
// helped create, through Atomics validation and .buffer; the main thread's
// view and buffer then alias the worker's buffer (wrapper identity across
// threads is a separate wrapper-cache concern and is not asserted).
for (let i = 0; i < ROUNDS; ++i) {
    const ta = new Int32Array(8);
    ta[5] = i;
    ta.a = i;
    let workerBuffer = null;
    new Thread(() => {
        ta.b = i;
        shouldBe(Atomics.load(ta, 5), i, "round " + i + ": worker Atomics.load on segmented view");
        workerBuffer = ta.buffer;
        shouldBe(workerBuffer.byteLength, 32, "round " + i + ": worker .buffer byteLength");
    }).join();
    shouldBe(ta.buffer.byteLength, 32, "round " + i + ": main .buffer after the worker's transition");
    ta[7] = 0x7e57 + i;
    shouldBe(new Int32Array(workerBuffer)[7], 0x7e57 + i, "round " + i + ": worker buffer aliases the view");
    shouldBe(new Int32Array(ta.buffer)[5], i, "round " + i + ": main buffer aliases the view");
    shouldBe([...ta].length, 8, "round " + i + ": spread over segmented Wasteful view");
}
