//@ runDefault("--useConcurrentGC=0")

// An eden collection frees the ArrayBuffers that only dead new objects referenced. The size of the heap has to stop
// counting them then. When it kept them until the next full collection, one eden's worth of dead buffers made the heap
// look as large as its limit, and every second collection of a program that allocates buffers was a full one.

function shouldBeTrue(condition, message) {
    if (!condition)
        throw new Error(message);
}

const MB = 1024 * 1024;
const count = 16;
const keptAlive = [];

// Less than the eden budget, so that no collection runs before the one the test asks for.
function allocateBuffers(keep) {
    for (let i = 0; i < count; i++) {
        const buffer = new ArrayBuffer(MB);
        if (i < keep)
            keptAlive.push(buffer);
    }
}
noInline(allocateBuffers);

function extraMemoryAfter(callback) {
    const before = $vm.heapExtraMemorySize();
    callback();
    return $vm.heapExtraMemorySize() - before;
}

function test() {
    fullGC();

    // Dead buffers stop counting in the eden collection that frees them. The conservative scan can keep the last one.
    let delta = extraMemoryAfter(() => {
        allocateBuffers(0);
        edenGC();
    });
    shouldBeTrue(delta < 2 * MB, `the heap holds ${delta} more bytes after an eden collection of ${count} dead 1 MB buffers`);

    // The ones that are alive count.
    delta = extraMemoryAfter(() => {
        allocateBuffers(4);
        edenGC();
    });
    shouldBeTrue(delta >= 4 * MB && delta < 6 * MB, `the heap holds ${delta} more bytes after an eden collection with 4 of ${count} 1 MB buffers alive`);

    // A buffer that an eden collection found alive is old. Only a full collection can find it dead.
    delta = extraMemoryAfter(() => {
        keptAlive.length = 0;
        edenGC();
    });
    shouldBeTrue(delta > -MB, `an eden collection took ${-delta} bytes of old buffers off the heap`);
    delta = extraMemoryAfter(() => {
        fullGC();
    });
    shouldBeTrue(delta <= -3 * MB, `a full collection took ${-delta} bytes of old buffers off the heap`);

    // The size of a buffer can change after it was counted: a transfer takes its contents away, resize() changes its
    // length. Whatever happens to the buffers, an eden collection must not leave the heap smaller than the collection
    // before it did (Heap::updateAllocationLimits() relies on that).
    let last = $vm.heapExtraMemorySize();
    function edenGCMustNotShrinkTheHeap(step) {
        edenGC();
        const now = $vm.heapExtraMemorySize();
        shouldBeTrue(now >= last, `the eden collection after ${step} took ${last - now} bytes off the heap`);
        last = now;
    }

    const resizable = new ArrayBuffer(4 * MB, { maxByteLength: 8 * MB });
    const moved = new ArrayBuffer(4 * MB);
    keptAlive.push(resizable, moved);
    edenGCMustNotShrinkTheHeap("allocating");
    resizable.resize(8 * MB);
    keptAlive.push(moved.transfer());
    edenGCMustNotShrinkTheHeap("growing one buffer and transferring another");
    resizable.resize(0);
    keptAlive.push(new ArrayBuffer(MB));
    edenGCMustNotShrinkTheHeap("shrinking an old buffer");
    keptAlive.length = 0;
    edenGCMustNotShrinkTheHeap("dropping everything");
    fullGC();
}

// Without the JIT the VM is in mini mode, where every collection is a full one.
if ($vm.useJIT())
    test();
