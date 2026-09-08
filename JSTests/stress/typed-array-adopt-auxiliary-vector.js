//@ requireOptions("--useDollarVM=1")
// JSGenericTypedArrayView::createWithAuxiliaryVector: a FastTypedArray over adopted GC-owned
// storage, past fastSizeLimit. It must behave like any Uint8Array through GC, JIT access,
// .buffer materialization, subarray/slice/set, and survive its creator.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("bad value: " + actual + " expected " + expected);
}

function sum(array) {
    let total = 0;
    for (let i = 0; i < array.length; ++i)
        total += array[i];
    return total;
}
noInline(sum);

function writeAll(array, value) {
    for (let i = 0; i < array.length; ++i)
        array[i] = value;
}
noInline(writeAll);

for (const length of [0, 1, 7, 1000, 1001, 4096, 100000, 3 * 1024 * 1024 + 3]) {
    const array = $vm.createUint8ArrayWithAuxiliaryVector(length, 7);
    shouldBe(array.length, length);
    shouldBe(array.byteLength, length);
    shouldBe(array.byteOffset, 0);
    shouldBe(array instanceof Uint8Array, true);
    shouldBe(sum(array), 7 * length);
    fullGC();
    shouldBe(sum(array), 7 * length);
    writeAll(array, 3);
    edenGC();
    shouldBe(sum(array), 3 * length);
    if (length) {
        shouldBe(array.subarray(length - 1)[0], 3);
        shouldBe(array.slice(-1)[0], 3);
        array.set([9], length - 1);
        shouldBe(array.at(-1), 9);
    }
    // Materializing the buffer moves the bytes into an ArrayBuffer; the view keeps working.
    const buffer = array.buffer;
    shouldBe(buffer.byteLength, length);
    shouldBe(new Uint8Array(buffer).at(-1), length ? 9 : undefined);
    if (length) {
        array[0] = 42;
        shouldBe(new Uint8Array(buffer)[0], 42);
    }
    fullGC();
    shouldBe(array.buffer, buffer);
    shouldBe(sum(array), !length ? 0 : length == 1 ? 42 : 3 * length + 6 + 39);
}

// Churn: many adopted vectors, most garbage, some retained across collections.
{
    const keep = [];
    for (let i = 0; i < 2000; ++i) {
        const length = 1001 + (i % 50) * 977;
        const array = $vm.createUint8ArrayWithAuxiliaryVector(length, i & 0xff);
        if (!(i % 97))
            keep.push([array, length, i & 0xff]);
        if (!(i % 500))
            edenGC();
    }
    fullGC();
    for (const [array, length, fill] of keep) {
        shouldBe(array.length, length);
        shouldBe(sum(array), fill * length);
    }
}

// JIT: hot typed-array loads/stores on an adopted vector, tiering up while GCs run.
{
    const array = $vm.createUint8ArrayWithAuxiliaryVector(65536, 1);
    for (let i = 0; i < 2000; ++i) {
        shouldBe(sum(array), 65536 * (i & 1 ? 2 : 1));
        writeAll(array, i & 1 ? 1 : 2);
        if (!(i % 300))
            fullGC();
    }
}
