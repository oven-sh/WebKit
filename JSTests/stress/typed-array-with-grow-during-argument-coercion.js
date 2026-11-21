// %TypedArray%.prototype.with reads `len` before it coerces `index` and `value`,
// and copies every element other than `index` from the source after the coercions.
// When a coercion grows the buffer under a length-tracking view, the copy must still
// carry the source's elements. BigInt64Array and BigUint64Array used to return a copy
// where every element except the replaced one was 0.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

function shouldThrow(fn, errorType, message) {
    let thrown;
    try {
        fn();
    } catch (e) {
        thrown = e;
    }
    if (!(thrown instanceof errorType))
        throw new Error(`${message}: expected ${errorType.name} but got ${thrown}`);
}

const constructors = [
    Int8Array,
    Uint8Array,
    Uint8ClampedArray,
    Int16Array,
    Uint16Array,
    Int32Array,
    Uint32Array,
    Float16Array,
    Float32Array,
    Float64Array,
    BigInt64Array,
    BigUint64Array,
];

function isBigInt(TA) {
    return TA === BigInt64Array || TA === BigUint64Array;
}

function isFloat(TA) {
    return TA === Float16Array || TA === Float32Array || TA === Float64Array;
}

function convert(TA, number) {
    return isBigInt(TA) ? BigInt(number) : number;
}

function makeBuffer(shared, byteLength, maxByteLength) {
    if (shared)
        return new SharedArrayBuffer(byteLength, { maxByteLength });
    return new ArrayBuffer(byteLength, { maxByteLength });
}

function growTo(buffer, byteLength) {
    if (buffer instanceof SharedArrayBuffer)
        buffer.grow(byteLength);
    else
        buffer.resize(byteLength);
}

function fill(view) {
    for (let i = 0; i < view.length; ++i)
        view[i] = convert(view.constructor, 10 + i);
}

function check(TA, copy, expected, message) {
    shouldBe(copy.constructor, TA, `${message}: constructor`);
    shouldBe(copy.length, expected.length, `${message}: length`);
    for (let i = 0; i < expected.length; ++i) {
        const want = convert(TA, expected[i]);
        const got = copy[i];
        if (Number.isNaN(want))
            shouldBe(Number.isNaN(got), true, `${message}: copy[${i}] is NaN`);
        else
            shouldBe(got, want, `${message}: copy[${i}]`);
    }
}

function test(TA, shared) {
    const bpe = TA.BYTES_PER_ELEMENT;
    const name = `${TA.name} over ${shared ? "growable SharedArrayBuffer" : "resizable ArrayBuffer"}`;

    // Coercing `value` grows the buffer.
    {
        const buffer = makeBuffer(shared, 4 * bpe, 8 * bpe);
        const view = new TA(buffer);
        fill(view);
        let calls = 0;
        const value = { valueOf() { ++calls; growTo(buffer, 6 * bpe); return convert(TA, 1); } };
        const copy = view.with(0, value);
        shouldBe(calls, 1, `${name}, value grows: valueOf calls`);
        shouldBe(view.length, 6, `${name}, value grows: source length afterwards`);
        check(TA, copy, [1, 11, 12, 13], `${name}, value grows`);
        shouldBe(copy.buffer.resizable, false, `${name}, value grows: copy.buffer.resizable`);
    }

    // Coercing `index` grows the buffer.
    {
        const buffer = makeBuffer(shared, 4 * bpe, 8 * bpe);
        const view = new TA(buffer);
        fill(view);
        let calls = 0;
        const index = { valueOf() { ++calls; growTo(buffer, 8 * bpe); return 2; } };
        const copy = view.with(index, convert(TA, 1));
        shouldBe(calls, 1, `${name}, index grows: valueOf calls`);
        check(TA, copy, [10, 11, 1, 13], `${name}, index grows`);
    }

    // Both grow, negative index, last element replaced.
    {
        const buffer = makeBuffer(shared, 4 * bpe, 8 * bpe);
        const view = new TA(buffer);
        fill(view);
        const index = { valueOf() { growTo(buffer, 5 * bpe); return -1; } };
        const value = { valueOf() { growTo(buffer, 7 * bpe); return convert(TA, 2); } };
        const copy = view.with(index, value);
        check(TA, copy, [10, 11, 12, 2], `${name}, both grow`);
    }

    // Length-tracking view with a byte offset.
    {
        const buffer = makeBuffer(shared, 4 * bpe, 8 * bpe);
        fill(new TA(buffer));
        const view = new TA(buffer, 1 * bpe);
        shouldBe(view.length, 3, `${name}, with offset: length before`);
        const value = { valueOf() { growTo(buffer, 8 * bpe); return convert(TA, 3); } };
        const copy = view.with(1, value);
        shouldBe(view.length, 7, `${name}, with offset: length after`);
        check(TA, copy, [11, 3, 13], `${name}, with offset`);
    }

    // The index is validated against the grown length, but an index at or past the
    // original length is still out of range for the copy, whose length is the original one.
    {
        const buffer = makeBuffer(shared, 4 * bpe, 8 * bpe);
        const view = new TA(buffer);
        fill(view);
        const index = { valueOf() { growTo(buffer, 8 * bpe); return 3; } };
        const copy = view.with(index, convert(TA, 4));
        check(TA, copy, [10, 11, 12, 4], `${name}, index grows to last`);
    }

    if (shared)
        return;

    // Shrinking inside the coercion, for contrast: elements past the new length read as
    // undefined. Number arrays store ToNumber(undefined), BigInt arrays throw on ToBigInt(undefined).
    {
        const buffer = makeBuffer(shared, 4 * bpe, 8 * bpe);
        const view = new TA(buffer);
        fill(view);
        const value = { valueOf() { buffer.resize(2 * bpe); return convert(TA, 5); } };
        if (isBigInt(TA))
            shouldThrow(() => view.with(1, value), TypeError, `${name}, value shrinks`);
        else
            check(TA, view.with(1, value), [10, 5, isFloat(TA) ? NaN : 0, isFloat(TA) ? NaN : 0], `${name}, value shrinks`);
    }

    // Shrinking so that the index itself goes out of bounds is a RangeError for every type.
    {
        const buffer = makeBuffer(shared, 4 * bpe, 8 * bpe);
        const view = new TA(buffer);
        fill(view);
        const value = { valueOf() { buffer.resize(2 * bpe); return convert(TA, 5); } };
        shouldThrow(() => view.with(3, value), RangeError, `${name}, value shrinks index out`);
    }
}

for (const TA of constructors) {
    for (let i = 0; i < 50; ++i) {
        test(TA, false);
        test(TA, true);
    }
}
