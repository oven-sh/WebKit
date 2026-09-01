//@ requireOptions("--useJSThreads=1", "--useThreadGIL=0", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1")
// GIL-off, detach()/transfer() leave the ArrayBuffer's base word in place
// until a stop-the-world retires the quarantined mapping. The JS-visible
// detached predicate must not depend on that: immediately after transfer(),
// with no GC in between, ArrayBuffer.prototype.detached reads true, view
// construction over the buffer throws, slice() throws, and resize() on a
// detached resizable buffer throws the detached TypeError rather than a
// RangeError.
load("../resources/assert.js", "caller relative");

for (let i = 0; i < 50; ++i) {
    const ab = new ArrayBuffer(16);
    const view = new Uint8Array(ab);
    view[3] = i;
    const transferred = ab.transfer();
    shouldBe(transferred.byteLength, 16, "round " + i + ": transferee byteLength");
    shouldBe(new Uint8Array(transferred)[3], i, "round " + i + ": transferee carries the data");
    shouldBeTrue(ab.detached, "round " + i + ": source is detached right after transfer");
    shouldBe(ab.byteLength, 0, "round " + i + ": detached byteLength");
    shouldBe(view.length, 0, "round " + i + ": view over the source is neutered");
    shouldThrow(TypeError, () => new Uint8Array(ab)); // View construction over the detached source.
    shouldThrow(TypeError, () => ab.slice(0));
    shouldThrow(TypeError, () => ab.transfer());

    const rab = new ArrayBuffer(16, { maxByteLength: 64 });
    rab.transfer();
    shouldBeTrue(rab.detached, "round " + i + ": resizable source is detached right after transfer");
    shouldThrow(TypeError, () => rab.resize(32)); // Detached TypeError, not the RangeError of a failed grow.
}
