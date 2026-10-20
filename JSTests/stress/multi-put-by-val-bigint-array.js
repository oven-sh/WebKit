// A put_by_val that has seen several kinds of typed arrays tests "is a typed array" once and ignores a store past the end.
// A BigInt64Array must not pass that test: storing a number into one throws whatever the index, since ToBigInt comes first.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("expected " + String(expected) + " but got " + String(actual));
}

function storePastEnd(array, index, value) {
    array[index] = value;
}
noInline(storePastEnd);

function storeBesideArrays(array, index, value) {
    array[index] = value;
}
noInline(storeBesideArrays);

function storeInBounds(array, index, value) {
    array[index] = value;
}
noInline(storeInBounds);

function storeResizable(array, index, value) {
    array[index] = value;
}
noInline(storeResizable);

function storeOtherViews(array, index, value) {
    array[index] = value;
}
noInline(storeOtherViews);

function attempt(store, array, index) {
    try {
        store(array, index, 7);
        return "stored";
    } catch (error) {
        return error.constructor.name;
    }
}
noInline(attempt);

function checkBigIntArraysThrow(store, buffer) {
    for (let i = 0; i < testLoopCount; ++i) {
        for (const array of [new BigInt64Array(buffer), new BigUint64Array(buffer)]) {
            shouldBe(attempt(store, array, 6), "TypeError");
            shouldBe(attempt(store, array, -1), "TypeError");
            shouldBe(attempt(store, array, 0), "TypeError");
            shouldBe(array[0], 0n);
        }
    }
}

const numberArrayConstructors = [Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array, Float32Array, Float64Array];
if (typeof Float16Array === "function")
    numberArrayConstructors.push(Float16Array);

function checkOtherViews(store) {
    for (let i = 0; i < 100; ++i) {
        for (const constructor of numberArrayConstructors) {
            const array = new constructor(4);
            shouldBe(attempt(store, array, 6), "stored");
            shouldBe(attempt(store, array, -1), "stored");
            shouldBe(array.length, 4);
            shouldBe(Object.keys(array).length, 4);
            shouldBe(attempt(store, array, 1), "stored");
            shouldBe(array[1], 7);
        }
        const view = new DataView(new ArrayBuffer(4));
        shouldBe(attempt(store, view, 0), "stored");
        shouldBe(view[0], 7);
        shouldBe(view.getInt8(0), 0);
    }
}

const plain = [0, 0, 0, 0];
plain[0] = 1;
const resizableInt8Array = new Int8Array(new ArrayBuffer(4, { maxByteLength: 16 }));
const resizableInt32Array = new Int32Array(new ArrayBuffer(16, { maxByteLength: 64 }));

for (let i = 0; i < testLoopCount * 10; ++i) {
    shouldBe(attempt(storePastEnd, new Uint8Array(4), 6), "stored");
    shouldBe(attempt(storePastEnd, new Float64Array(4), 6), "stored");
    shouldBe(attempt(storeBesideArrays, plain, 1), "stored");
    shouldBe(attempt(storeBesideArrays, new Int16Array(4), 6), "stored");
    shouldBe(attempt(storeBesideArrays, new Uint32Array(4), 6), "stored");
    shouldBe(attempt(storeInBounds, new Int8Array(4), i & 3), "stored");
    shouldBe(attempt(storeInBounds, new Float32Array(4), i & 3), "stored");
    shouldBe(attempt(storeResizable, resizableInt8Array, 6), "stored");
    shouldBe(attempt(storeResizable, resizableInt32Array, 6), "stored");
    shouldBe(attempt(storeOtherViews, new Uint16Array(4), 6), "stored");
    shouldBe(attempt(storeOtherViews, new Int32Array(4), 6), "stored");
}

checkBigIntArraysThrow(storePastEnd, new ArrayBuffer(32));
checkBigIntArraysThrow(storeBesideArrays, new ArrayBuffer(32));
checkBigIntArraysThrow(storeInBounds, new ArrayBuffer(32));
checkBigIntArraysThrow(storeResizable, new ArrayBuffer(32, { maxByteLength: 64 }));

checkOtherViews(storeOtherViews);
checkOtherViews(storePastEnd);
checkOtherViews(storeBesideArrays);
checkOtherViews(storeInBounds);
checkOtherViews(storeResizable);
