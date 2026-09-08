function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`actual: ${actual}, expected: ${expected}`);
}

function shouldBeArray(actual, expected) {
    shouldBe(Array.from(actual).join(), expected.join());
}

function shouldThrow(func, errorType, message) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof errorType))
        throw new Error(`Expected ${errorType.name} but got ${error}`);
    if (message !== undefined)
        shouldBe(error.message, message);
}

const toBigIntError = "Invalid argument type in ToBigInt operation";

// %TypedArray%.prototype.filter reads each element with TypedArrayGetElement before it calls the callback,
// so once the callback shrinks or detaches the buffer the remaining elements are undefined. The kept
// values are then stored into the new array with TypedArraySetElement, which is ToBigInt(value) for a
// BigInt array. ToBigInt(undefined) throws a TypeError.

for (const TA of [BigInt64Array, BigUint64Array]) {
    // Shrink to zero mid-iteration, keep everything.
    {
        const buffer = new ArrayBuffer(40, { maxByteLength: 40 });
        const array = new TA(buffer).fill(7n);
        const seen = [];
        shouldThrow(() => array.filter((x, i) => { seen.push(x); if (i === 2) buffer.resize(0); return true; }), TypeError, toBigIntError);
        shouldBeArray(seen, [7n, 7n, 7n, undefined, undefined]);
    }

    // Shrink partially: only the elements past the new length are undefined.
    {
        const buffer = new ArrayBuffer(40, { maxByteLength: 40 });
        const array = new TA(buffer).fill(7n);
        shouldThrow(() => array.filter((x, i) => { if (i === 0) buffer.resize(32); return true; }), TypeError, toBigIntError);
    }
    {
        const buffer = new ArrayBuffer(40, { maxByteLength: 40 });
        const array = new TA(buffer).fill(7n);
        shouldBeArray(array.filter((x, i) => { if (i === 0) buffer.resize(32); return i < 4; }), [7n, 7n, 7n, 7n]);
    }

    // A fixed-length view on a resizable buffer goes out of bounds as a whole.
    {
        const buffer = new ArrayBuffer(40, { maxByteLength: 40 });
        const array = new TA(buffer, 8, 2).fill(7n);
        const seen = [];
        shouldThrow(() => array.filter((x, i) => { seen.push(x); if (i === 0) buffer.resize(16); return true; }), TypeError, toBigIntError);
        shouldBeArray(seen, [7n, undefined]);
    }

    // Detach mid-iteration.
    {
        const buffer = new ArrayBuffer(40, { maxByteLength: 40 });
        const array = new TA(buffer).fill(7n);
        shouldThrow(() => array.filter((x, i) => { if (i === 2) buffer.transfer(); return true; }), TypeError, toBigIntError);
    }
    {
        const buffer = new ArrayBuffer(40);
        const array = new TA(buffer).fill(7n);
        shouldThrow(() => array.filter((x, i) => { if (i === 2) buffer.transfer(); return true; }), TypeError, toBigIntError);
    }

    // Dropping the undefined elements is fine: nothing undefined is stored.
    {
        const buffer = new ArrayBuffer(40, { maxByteLength: 40 });
        const array = new TA(buffer).fill(7n);
        const result = array.filter((x, i) => { if (i === 2) buffer.resize(0); return x !== undefined; });
        shouldBeArray(result, [7n, 7n, 7n]);
        shouldBe(result.buffer.resizable, false);
    }
    {
        const buffer = new ArrayBuffer(40, { maxByteLength: 40 });
        const array = new TA(buffer).fill(7n);
        shouldBeArray(array.filter((x, i) => { if (i === 0) buffer.transfer(); return i === 0; }), [7n]);
    }

    // Shrink, then grow back: the elements read while out of bounds stay undefined, the later ones are 0n.
    {
        const buffer = new ArrayBuffer(40, { maxByteLength: 40 });
        const array = new TA(buffer).fill(7n);
        const seen = [];
        shouldThrow(() => array.filter((x, i) => {
            seen.push(x);
            if (i === 1)
                buffer.resize(8);
            if (i === 2)
                buffer.resize(40);
            return true;
        }), TypeError, toBigIntError);
        shouldBeArray(seen, [7n, 7n, undefined, 0n, 0n]);
    }

    // A callback that is not a JSFunction takes the generic call path.
    {
        const buffer = new ArrayBuffer(40, { maxByteLength: 40 });
        const array = new TA(buffer).fill(7n);
        const callback = new Proxy(function (x, i) { if (i === 2) buffer.resize(0); return true; }, {});
        shouldThrow(() => array.filter(callback), TypeError, toBigIntError);
    }
    {
        const buffer = new ArrayBuffer(40, { maxByteLength: 40 });
        const array = new TA(buffer).fill(7n);
        const callback = new Proxy(function (x, i) { if (i === 2) buffer.resize(0); return x !== undefined; }, {});
        shouldBeArray(array.filter(callback), [7n, 7n, 7n]);
    }

    // Order of operations: every callback runs, then TypedArraySpeciesCreate runs with the number of kept
    // elements, then the kept elements are stored in order until the first undefined one throws. The
    // elements of the new array from that index on are left as they were.
    {
        const log = [];
        let created;
        class Derived extends TA {
            constructor(...args)
            {
                super(...args);
                log.push(`construct ${args[0]}`);
                created = this;
            }
        }
        const buffer = new ArrayBuffer(40, { maxByteLength: 40 });
        const array = new Derived(buffer);
        log.length = 0;
        array.fill(7n);
        shouldThrow(() => array.filter((x, i) => {
            log.push(`callback ${i} ${x}`);
            if (i === 1)
                buffer.resize(8);
            if (i === 2)
                buffer.resize(40);
            return true;
        }), TypeError, toBigIntError);
        shouldBe(log.join(", "), "callback 0 7, callback 1 7, callback 2 undefined, callback 3 0, callback 4 0, construct 5");
        shouldBe(created.length, 5);
        shouldBeArray(created, [7n, 7n, 0n, 0n, 0n]);
    }
    {
        const other = new TA(8).fill(3n);
        const buffer = new ArrayBuffer(40, { maxByteLength: 40 });
        const array = new TA(buffer).fill(7n);
        array.constructor = { [Symbol.species]: function () { return other; } };
        shouldThrow(() => array.filter((x, i) => { if (i === 1) buffer.resize(0); return true; }), TypeError, toBigIntError);
        shouldBeArray(other, [7n, 7n, 3n, 3n, 3n, 3n, 3n, 3n]);
    }

    // The other BigInt type as species: same content type, so the prefix is still stored before the throw.
    {
        const OtherTA = TA === BigInt64Array ? BigUint64Array : BigInt64Array;
        let created;
        const buffer = new ArrayBuffer(40, { maxByteLength: 40 });
        const array = new TA(buffer).fill(7n);
        array.constructor = { [Symbol.species]: function (n) { return created = new OtherTA(n); } };
        shouldThrow(() => array.filter((x, i) => { if (i === 2) buffer.resize(0); return true; }), TypeError, toBigIntError);
        shouldBe(created.constructor, OtherTA);
        shouldBeArray(created, [7n, 7n, 7n, 0n, 0n]);
    }

    // No resize: unchanged.
    {
        const buffer = new ArrayBuffer(40, { maxByteLength: 80 });
        const array = new TA(buffer);
        for (let i = 0; i < array.length; ++i)
            array[i] = BigInt(i);
        shouldBeArray(array.filter(x => x & 1n), [1n, 3n]);
        buffer.resize(80);
        shouldBeArray(array.filter(x => x > 2n), [3n, 4n]);
    }
}

// Number typed arrays store ToNumber(undefined), NaN, converted to the element type. That does not throw.
for (const [TA, expected] of [
    [Float64Array, [7, 7, 7, NaN, NaN]],
    [Float32Array, [7, 7, 7, NaN, NaN]],
    [Float16Array, [7, 7, 7, NaN, NaN]],
    [Int32Array, [7, 7, 7, 0, 0]],
    [Uint32Array, [7, 7, 7, 0, 0]],
    [Int16Array, [7, 7, 7, 0, 0]],
    [Uint8Array, [7, 7, 7, 0, 0]],
    [Uint8ClampedArray, [7, 7, 7, 0, 0]],
    [Int8Array, [7, 7, 7, 0, 0]],
]) {
    const size = 5 * TA.BYTES_PER_ELEMENT;
    {
        const buffer = new ArrayBuffer(size, { maxByteLength: size });
        const array = new TA(buffer).fill(7);
        shouldBeArray(array.filter((x, i) => { if (i === 2) buffer.resize(0); return true; }), expected);
    }
    {
        const buffer = new ArrayBuffer(size, { maxByteLength: size });
        const array = new TA(buffer).fill(7);
        shouldBeArray(array.filter((x, i) => { if (i === 2) buffer.transfer(); return true; }), expected);
    }
}
