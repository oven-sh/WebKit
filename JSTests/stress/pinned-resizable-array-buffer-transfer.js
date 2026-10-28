// Native code that holds a pointer into the bytes of an ArrayBuffer pins the buffer: ArrayBuffer::pin(), which
// $vm.pinArrayBuffer() calls. A transfer of a pinned buffer copies the bytes and leaves the buffer attached. The copy
// of a resizable buffer has to be resizable with the same maximum, as the contents of a buffer that is not pinned are:
// ArrayBuffer.prototype.transfer() resizes what it gets.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

function shouldThrow(callback, errorConstructor, message) {
    let error;
    try {
        callback();
    } catch (thrown) {
        error = thrown;
    }
    if (!(error instanceof errorConstructor))
        throw new Error(`${message}: expected a ${errorConstructor.name} but got ${error}`);
}

function shape(buffer) {
    return `${buffer.byteLength} of ${buffer.maxByteLength}, ${buffer.resizable ? "resizable" : "fixed"}, ${buffer.detached ? "detached" : "attached"}`;
}

function bytes(buffer) {
    return Array.from(new Uint8Array(buffer)).join(",");
}

function makeBuffer(length, options) {
    const buffer = new ArrayBuffer(length, options);
    const view = new Uint8Array(buffer);
    for (let i = 0; i < view.length; i++)
        view[i] = i + 1;
    return buffer;
}

const first16 = "1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16";
const zeros16 = "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0";

function testPinnedResizable() {
    for (const [newLength, expectedBytes] of [
        [undefined, first16],
        [0, ""],
        [8, "1,2,3,4,5,6,7,8"],
        [16, first16],
        [32, `${first16},${zeros16}`],
        [64, `${first16},${zeros16},${zeros16},${zeros16}`],
    ]) {
        const source = makeBuffer(16, { maxByteLength: 64 });
        $vm.pinArrayBuffer(source);
        const result = source.transfer(newLength);
        const expectedLength = newLength ?? 16;
        shouldBe(shape(result), `${expectedLength} of 64, resizable, attached`, `transfer(${newLength}) of a pinned resizable buffer`);
        shouldBe(bytes(result), expectedBytes, `bytes after transfer(${newLength})`);
        shouldBe(shape(source), "16 of 64, resizable, attached", `the pinned source of transfer(${newLength})`);
        shouldBe(bytes(source), first16, `bytes of the pinned source of transfer(${newLength})`);

        // The result is a buffer of its own.
        result.resize(64);
        new Uint8Array(result)[0] = 99;
        shouldBe(new Uint8Array(source)[0], 1, `the pinned source after a write to the result of transfer(${newLength})`);
        shouldBe(shape(source), "16 of 64, resizable, attached", `the pinned source after the result of transfer(${newLength}) grew`);
        $vm.unpinArrayBuffer(source);
    }

    const source = makeBuffer(16, { maxByteLength: 64 });
    $vm.pinArrayBuffer(source);
    shouldThrow(() => source.transfer(65), RangeError, "transfer() of a pinned resizable buffer past its maximum");
    shouldBe(shape(source), "16 of 64, resizable, attached", "the pinned source of a transfer() past its maximum");
    $vm.unpinArrayBuffer(source);
}

function testPinnedResizableResizedWhilePinned() {
    const source = makeBuffer(16, { maxByteLength: 64 });
    $vm.pinArrayBuffer(source);
    source.resize(32);
    const result = source.transfer();
    shouldBe(shape(result), "32 of 64, resizable, attached", "transfer() of a pinned buffer that grew");
    shouldBe(bytes(result), `${first16},${zeros16}`, "bytes after transfer() of a pinned buffer that grew");
    $vm.unpinArrayBuffer(source);
}

// A maximum above 64 KiB: the pages past the length are reserved, and not accessible until the copy grows into them.
function testPinnedResizableWithPagesInReserve() {
    const maxByteLength = 1024 * 1024;
    const source = makeBuffer(16, { maxByteLength });
    $vm.pinArrayBuffer(source);
    const result = source.transfer();
    shouldBe(shape(result), `16 of ${maxByteLength}, resizable, attached`, "transfer() of a pinned buffer with a 1 MiB maximum");
    result.resize(maxByteLength);
    const view = new Uint8Array(result);
    view[maxByteLength - 1] = 7;
    shouldBe(view[0] + view[15] + view[16] + view[maxByteLength - 1], 1 + 16 + 0 + 7, "bytes of the copy after it grew to its maximum");
    shouldBe(shape(source), `16 of ${maxByteLength}, resizable, attached`, "the pinned source with a 1 MiB maximum");
    $vm.unpinArrayBuffer(source);
}

function testPinnedEmptyResizable() {
    const source = new ArrayBuffer(0, { maxByteLength: 64 });
    $vm.pinArrayBuffer(source);
    const result = source.transfer();
    shouldBe(shape(result), "0 of 64, resizable, attached", "transfer() of a pinned empty resizable buffer");
    result.resize(64);
    shouldBe(shape(source), "0 of 64, resizable, attached", "the pinned empty source");
    $vm.unpinArrayBuffer(source);
}

function testPinnedToFixedLength() {
    for (const options of [{ maxByteLength: 64 }, undefined]) {
        const kind = options ? "resizable" : "fixed-length";
        const sourceShape = options ? "16 of 64, resizable, attached" : "16 of 16, fixed, attached";
        for (const [newLength, expectedBytes] of [
            [undefined, first16],
            [0, ""],
            [8, "1,2,3,4,5,6,7,8"],
            [32, `${first16},${zeros16}`],
        ]) {
            const source = makeBuffer(16, options);
            $vm.pinArrayBuffer(source);
            const result = source.transferToFixedLength(newLength);
            const expectedLength = newLength ?? 16;
            shouldBe(shape(result), `${expectedLength} of ${expectedLength}, fixed, attached`, `transferToFixedLength(${newLength}) of a pinned ${kind} buffer`);
            shouldBe(bytes(result), expectedBytes, `bytes after transferToFixedLength(${newLength}) of a pinned ${kind} buffer`);
            shouldBe(shape(source), sourceShape, `the pinned ${kind} source of transferToFixedLength(${newLength})`);
            shouldBe(bytes(source), first16, `bytes of the pinned ${kind} source of transferToFixedLength(${newLength})`);
            $vm.unpinArrayBuffer(source);
        }
    }
}

function testPinnedFixedLength() {
    for (const [newLength, expectedBytes] of [
        [undefined, first16],
        [0, ""],
        [8, "1,2,3,4,5,6,7,8"],
        [32, `${first16},${zeros16}`],
    ]) {
        const source = makeBuffer(16);
        $vm.pinArrayBuffer(source);
        const result = source.transfer(newLength);
        const expectedLength = newLength ?? 16;
        shouldBe(shape(result), `${expectedLength} of ${expectedLength}, fixed, attached`, `transfer(${newLength}) of a pinned fixed-length buffer`);
        shouldBe(bytes(result), expectedBytes, `bytes after transfer(${newLength}) of a pinned fixed-length buffer`);
        shouldBe(shape(source), "16 of 16, fixed, attached", `the pinned fixed-length source of transfer(${newLength})`);
        $vm.unpinArrayBuffer(source);
    }
}

// Made in a frame that is gone when they are freed, so that only the array refers to them.
function makeBuffersToFree() {
    return [new ArrayBuffer(1024 * 1024), new ArrayBuffer(1024 * 1024, { maxByteLength: 1024 * 1024 })];
}

// A resizable buffer takes its pages from a budget of three times the memory of the machine, or from the Gigacage where
// there is one. This fills both, and then frees a little.
function testWithLittleRoom() {
    const large = makeBuffer(16, { maxByteLength: 2 ** 32 });
    const small = makeBuffer(16, { maxByteLength: 65536 });
    $vm.pinArrayBuffer(large);
    $vm.pinArrayBuffer(small);
    // The first is fixed-length: without it a full Gigacage has no room for the small fixed-length buffers below. The
    // second is resizable: it is the room for 16 copies of `small`, or for 32 in a Gigacage.
    const toFree = makeBuffersToFree();
    const held = [];
    for (let size = 2 ** 32; size >= 65536; size /= 2) {
        try {
            for (let i = 0; i < 4096; i++)
                held.push(new ArrayBuffer(size, { maxByteLength: size }));
        } catch { }
    }
    toFree[0] = null;
    gc();

    // With no room for the copy, transfer() throws and leaves the buffer as it is. transferToFixedLength() makes no
    // such copy.
    for (const newLength of [undefined, 8, 32]) {
        shouldThrow(() => large.transfer(newLength), RangeError, `transfer(${newLength}) of a pinned resizable buffer with no room for the copy`);
        shouldBe(shape(large), `16 of ${2 ** 32}, resizable, attached`, `the pinned source of a transfer(${newLength}) that failed`);
    }
    shouldBe(shape(large.transferToFixedLength()), "16 of 16, fixed, attached", "transferToFixedLength() with no room for a resizable copy");
    shouldBe(shape(large.transferToFixedLength(32)), "32 of 32, fixed, attached", "transferToFixedLength(32) with no room for a resizable copy");
    shouldBe(bytes(large), first16, "bytes of the pinned source with no room for a resizable copy");

    // With room for a few copies, the copies that nothing holds any more are collected to make the next one.
    toFree[1] = null;
    gc();
    for (let i = 0; i < 64; i++)
        shouldBe(shape(small.transfer()), "16 of 65536, resizable, attached", `transfer() number ${i} with room for a few copies`);

    $vm.unpinArrayBuffer(large);
    $vm.unpinArrayBuffer(small);
    held.length = 0;
}

// Each pin counts. The buffer moves again once the last one is gone.
function testUnpinned() {
    for (const transfer of [buffer => buffer.transfer(), buffer => buffer.transfer(32), buffer => buffer.transferToFixedLength(), buffer => buffer.transferToFixedLength(32)]) {
        for (const options of [{ maxByteLength: 64 }, undefined]) {
            const source = makeBuffer(16, options);
            $vm.pinArrayBuffer(source);
            $vm.pinArrayBuffer(source);
            $vm.unpinArrayBuffer(source);
            shouldBe(transfer(source).detached, false, "the result of a transfer of a buffer that is pinned once more");
            shouldBe(source.detached, false, "a buffer that is pinned once more");
            $vm.unpinArrayBuffer(source);
            const result = transfer(source);
            shouldBe(source.detached, true, "a buffer that is not pinned any more");
            shouldBe(source.byteLength, 0, "the length of a buffer that is not pinned any more");
            shouldBe(bytes(result).slice(0, first16.length), first16, "bytes of a buffer that is not pinned any more");
        }
    }
    shouldThrow(() => $vm.unpinArrayBuffer(new ArrayBuffer(16)), TypeError, "unpinArrayBuffer() of a buffer that is not pinned");
}

testPinnedResizable();
testPinnedResizableResizedWhilePinned();
testPinnedResizableWithPagesInReserve();
testPinnedEmptyResizable();
testPinnedToFixedLength();
testPinnedFixedLength();
testUnpinned();
testWithLittleRoom();
