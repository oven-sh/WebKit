//@ runDefault("--useConcurrentGC=0")

// A typed array past the fast size limit owns a malloc'ed vector, and it reports the vector to the heap when it
// allocates it. Reading .buffer, or calling subarray(), wraps that same vector in an ArrayBuffer. Nothing of that size
// is allocated then, so the heap must not count the vector as allocated a second time: collections are paced on the
// bytes allocated, and a program that does this to every array it makes would collect twice as often.

function shouldBeTrue(condition, message) {
    if (!condition)
        throw new Error(message);
}

const size = 4 * 1024 * 1024;
const slack = 1024 * 1024;
const keptAlive = [];

// $vm.heapTotalBytesAllocated() is what the heap has counted as allocated so far, cells and extra memory alike.
function bytesAllocatedBy(callback) {
    const before = $vm.heapTotalBytesAllocated();
    const result = callback();
    const after = $vm.heapTotalBytesAllocated();
    keptAlive.push(result);
    return [after - before, result];
}

function test() {
    const [toCreate, array] = bytesAllocatedBy(() => new Uint8Array(size));
    shouldBeTrue(toCreate >= size, `creating the array allocated ${toCreate} bytes, less than its ${size}`);
    shouldBeTrue(toCreate < size + slack, `creating the array allocated ${toCreate} bytes`);

    const [toWrap, buffer] = bytesAllocatedBy(() => array.buffer);
    shouldBeTrue(buffer.byteLength === size, `the buffer is ${buffer.byteLength} bytes long`);
    shouldBeTrue(toWrap < slack, `reading .buffer of an oversize array allocated ${toWrap} bytes`);

    const [toCreateAndSlice, slice] = bytesAllocatedBy(() => new Float64Array(size / 8).subarray(1, 2));
    shouldBeTrue(slice.buffer.byteLength === size, `the slice's buffer is ${slice.buffer.byteLength} bytes long`);
    shouldBeTrue(toCreateAndSlice >= size, `creating the array and its slice allocated ${toCreateAndSlice} bytes, less than the array's ${size}`);
    shouldBeTrue(toCreateAndSlice < size + slack, `creating the array and its slice allocated ${toCreateAndSlice} bytes`);

    // A fast typed array has its vector in the GC heap. Its buffer is a copy in the C heap, which is an allocation.
    const [, small] = bytesAllocatedBy(() => new Uint8Array(1000));
    const [toCopy, smallBuffer] = bytesAllocatedBy(() => small.buffer);
    shouldBeTrue(smallBuffer.byteLength === 1000, `the small buffer is ${smallBuffer.byteLength} bytes long`);
    shouldBeTrue(toCopy >= 1000, `reading .buffer of a fast array allocated ${toCopy} bytes`);
}

// The size of the heap has the vector once, whether the array or its buffer carries it.
function testHeapSize() {
    fullGC();
    const before = $vm.heapExtraMemorySize();
    const array = new Uint8Array(size);
    keptAlive.push(array);
    fullGC();
    const asArray = $vm.heapExtraMemorySize() - before;
    shouldBeTrue(Math.abs(asArray - size) < slack, `the heap holds ${asArray} more bytes with the array`);

    keptAlive.push(array.buffer);
    fullGC();
    const asBuffer = $vm.heapExtraMemorySize() - before;
    shouldBeTrue(Math.abs(asBuffer - size) < slack, `the heap holds ${asBuffer} more bytes with the array and its buffer`);
}

test();
testHeapSize();
