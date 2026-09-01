//@ requireOptions("--useJSThreads=1")
// r47 (FUZZ.md §47, SCALEBENCH.md §47-NEW-residual): JSArrayBufferView::
// slowDownAndWasteMemory was a manifest-7 owner-TID audit escape - a worker
// reading .buffer / Atomics-validating a Fast/OversizeTypedArray created on
// another thread allocated the wastage butterfly and setButterfly()'d it with
// no protocol, tripping storeTaggedButterflyWordConcurrent's owner-TID
// RELEASE_ASSERT (JSObjectInlines.h:116) when the view already carried a
// foreign-TID butterfly word, and (independently) publishing the butterfly
// BEFORE its IndexingHeader::arrayBuffer slot was filled, so concurrent
// existingBufferInButterfly() readers derefed ASAN poison (r47-002 SEGV in
// DeferrableRefCountedBase::ref).
//
// Covers, 100 rounds each:
//   (1) worker .buffer on a main-created FastTypedArray with NO butterfly
//       (None word -> N3 first install on the worker);
//   (2) worker .buffer on a main-created FastTypedArray that ALREADY has an
//       out-of-line named property (foreign-TID flat word -> tag-preserving
//       cell-locked CAS; the r47-001 trap site);
//   (3) worker Atomics.load on a main-created FastTypedArray (the
//       validateTypedArray -> isArrayBufferViewOutOfBounds path);
//   (4) two workers racing .buffer on the SAME main-created view (cell-locked
//       idempotency: both must see the same ArrayBuffer, no double-adopt).
load("../harness.js", "caller relative");

const ROUNDS = 100;

// (1) None word: worker performs the wastage install as an N3 first install.
for (let i = 0; i < ROUNDS; ++i) {
    const ta = new Int32Array(8); // FastTypedArray (small, no explicit buffer, no named props).
    ta[3] = 0x1234 + i;
    const workers = spawnN(1, () => {
        const buf = ta.buffer; // slowDownAndWasteMemory on the worker.
        shouldBeTrue(buf instanceof ArrayBuffer, "round " + i + ": .buffer is ArrayBuffer");
        shouldBe(buf.byteLength, 32, "round " + i + ": .buffer byteLength");
    });
    joinAll(workers);
    shouldBe(ta.buffer.byteLength, 32, "round " + i + ": post-join .buffer");
    shouldBe(ta[3], 0x1234 + i, "round " + i + ": element survived wastage copy");
}

// (2) Foreign-TID flat word: main installs an out-of-line named property
// (butterfly tagged with main's TID), worker grows it for the wastage header.
// This is the r47-001 storeTaggedButterflyWordConcurrent owner-TID trap.
for (let i = 0; i < ROUNDS; ++i) {
    const ta = new Int32Array(8);
    ta[0] = 0x4321 + i;
    ta.namedProp = i; // out-of-line on main -> butterfly word tagged with main's TID.
    const workers = spawnN(1, () => {
        const buf = ta.buffer; // foreign-TID slowDownAndWasteMemory on the worker.
        shouldBeTrue(buf instanceof ArrayBuffer, "round " + i + ": foreign-TID .buffer");
        shouldBe(buf.byteLength, 32, "round " + i + ": foreign-TID byteLength");
    });
    joinAll(workers);
    shouldBe(ta.namedProp, i, "round " + i + ": named prop survived wastage grow");
    shouldBe(ta[0], 0x4321 + i, "round " + i + ": element survived foreign-TID wastage copy");
}

// (3) Atomics.load: ValidateIntegerTypedArray -> isArrayBufferViewOutOfBounds
// on a main-created FastTypedArray from a worker (the r47-002 reader path).
for (let i = 0; i < ROUNDS; ++i) {
    const ta = new Int32Array(8);
    ta[5] = i;
    const workers = spawnN(1, () => {
        shouldBe(Atomics.load(ta, 5), i, "round " + i + ": Atomics.load on foreign FastTypedArray");
    });
    joinAll(workers);
}

// (4) Idempotency under contention: two workers race .buffer on the same
// main-created FastTypedArray. The cell-locked re-check guarantees one C++
// ArrayBuffer wins; verify by aliasing (a post-join store through ta is
// visible through BOTH workers' buffer values). JS-wrapper identity is NOT
// asserted here - typedArrayController->toJS wrapper-cache races are a
// separate concern outside the r47 publication-order fix.
for (let i = 0; i < ROUNDS; ++i) {
    const ta = new Int32Array(8);
    ta[1] = 0x5a5a + i;
    const seen = [null, null];
    const workers = spawnN(2, (t) => {
        seen[t] = ta.buffer;
        shouldBe(Atomics.load(ta, 1), 0x5a5a + i, "round " + i + " t" + t + ": racing Atomics.load");
    });
    joinAll(workers);
    shouldBeTrue(seen[0] instanceof ArrayBuffer, "round " + i + ": racer 0 got ArrayBuffer");
    shouldBeTrue(seen[1] instanceof ArrayBuffer, "round " + i + ": racer 1 got ArrayBuffer");
    shouldBe(seen[0].byteLength, 32, "round " + i + ": racer 0 byteLength");
    shouldBe(seen[1].byteLength, 32, "round " + i + ": racer 1 byteLength");
    ta[7] = 0x7e57 + i; // post-join write through the (now Wasteful) view.
    shouldBe(new Int32Array(seen[0])[7], 0x7e57 + i, "round " + i + ": racer 0 buffer aliases ta");
    shouldBe(new Int32Array(seen[1])[7], 0x7e57 + i, "round " + i + ": racer 1 buffer aliases ta");
}
