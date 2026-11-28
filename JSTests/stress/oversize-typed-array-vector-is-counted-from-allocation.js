//@ runDefault("--useConcurrentGC=0")

// A typed array past the fast size limit owns a malloc'ed vector with no ArrayBuffer. The heap keeps an exact count of
// those vectors from the moment one is allocated: $vm.heapTypedArrayBackingStoreSize() and
// $vm.heapExtraMemorySizeForReporting() see it at once, see it once after .buffer hands it to an ArrayBuffer, and see
// it gone once the array is collected. $vm.heapExtraMemorySize(), which paces collections, is unchanged: it counts the
// vector from the marking that visits it.

function shouldBeTrue(condition, message) {
    if (!condition)
        throw new Error(message);
}

function shouldBeNear(value, expected, message) {
    shouldBeTrue(Math.abs(value - expected) < slack, `${message}: ${value}, expected about ${expected}`);
}

function shouldBeAtLeast(value, expected, message) {
    shouldBeTrue(value >= expected, `${message}: ${value}, expected at least ${expected}`);
}

const size = 4 * 1024 * 1024;
const count = 8;
const slack = 1024 * 1024;

function sample() {
    return {
        backingStore: $vm.heapTypedArrayBackingStoreSize(),
        reported: $vm.heapExtraMemorySizeForReporting(),
        paced: $vm.heapExtraMemorySize(),
    };
}

// Only this array references the typed arrays. They are made and wrapped in separate frames, which keeps stale
// references off the stack where the conservative scan would find them.
let held = [];

function allocate(make) {
    for (let i = 0; i < count; ++i)
        held.push(make());
}
noInline(allocate);

// Every other array hands its vector to an ArrayBuffer.
function wrapHalf() {
    for (let i = 0; i < count; i += 2)
        held[i].buffer;
}
noInline(wrapHalf);

function test(make, bytesEach) {
    fullGC();
    const payload = bytesEach * count;
    const before = sample();

    allocate(make);
    const created = sample();
    shouldBeNear(created.backingStore - before.backingStore, payload, "backing store after creating the arrays");
    shouldBeNear(created.reported - before.reported, payload, "reported extra memory after creating the arrays");

    fullGC();
    const marked = sample();
    shouldBeNear(marked.backingStore - before.backingStore, payload, "backing store after a collection");
    shouldBeNear(marked.reported - before.reported, payload, "reported extra memory after a collection");
    shouldBeNear(marked.paced - before.paced, payload, "paced extra memory after a collection");

    // The bytes move between counters and are never doubled.
    wrapHalf();
    const wrapped = sample();
    shouldBeNear(wrapped.backingStore - before.backingStore, payload, "backing store after reading .buffer");
    shouldBeNear(wrapped.reported - before.reported, payload, "reported extra memory after reading .buffer");

    fullGC();
    const markedAgain = sample();
    shouldBeNear(markedAgain.paced - before.paced, payload, "paced extra memory after reading .buffer and collecting");

    // The arrays die: the ones with a buffer leave through the ArrayBuffer sweep, the others through their finalizer,
    // which runs when the collection sweeps (gc() sweeps at once, fullGC() leaves it to later). Allow for a few arrays
    // still found by the conservative stack scan.
    held.length = 0;
    gc();
    const released = sample();
    const mostOfThem = payload - 2 * bytesEach;
    shouldBeAtLeast(marked.backingStore - released.backingStore, mostOfThem, "backing store given back once the arrays are dead");
    shouldBeAtLeast(marked.reported - released.reported, mostOfThem, "reported extra memory given back once the arrays are dead");
    shouldBeAtLeast(marked.paced - released.paced, mostOfThem, "paced extra memory given back once the arrays are dead");
}

test(() => new Uint8Array(size), size);
test(() => new Float64Array(size / 8), size);

// A fast typed array has its vector in the GC heap, so it is part of the heap's own size and not of these counts.
function testFastArrays() {
    fullGC();
    const before = sample();
    for (let i = 0; i < 1024; ++i)
        held.push(new Uint8Array(1000));
    const created = sample();
    shouldBeTrue(created.backingStore - before.backingStore < slack, `backing store counts fast arrays: ${created.backingStore - before.backingStore}`);
    shouldBeTrue(created.reported - before.reported < slack, `reported extra memory counts fast arrays: ${created.reported - before.reported}`);
    held.length = 0;
}

testFastArrays();
