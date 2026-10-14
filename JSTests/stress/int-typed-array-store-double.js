function shouldBe(actual, expected, message)
{
    if (!Object.is(actual, expected))
        throw new Error(`${message}: got ${actual}, expected ${expected}`);
}

const intTypes = [Int8Array, Uint8Array, Int16Array, Uint16Array, Int32Array, Uint32Array];

// What an element of the given type holds after a store, through BigInt, independent of the conversion under test.
function reference(type, value)
{
    if (!Number.isFinite(value))
        return 0;
    const bits = type.BYTES_PER_ELEMENT * 8;
    const integer = BigInt(Math.trunc(value));
    return Number(type.name.startsWith("U") ? BigInt.asUintN(bits, integer) : BigInt.asIntN(bits, integer));
}

const doubles = [
    0.5, -0.5, 1.5, -1.5, 127.9, 128.5, -128.9, -129.5, 255.5, 256.5, 32767.5, 65535.5, 65536.5, 301000.5,
    2147483647.9, 2147483648.5, -2147483648.9, -2147483649.5, 4294967295.5, 4294967296.5,
    2 ** 52 + 1, 2 ** 53 + 2, 2 ** 62, -(2 ** 62), 2 ** 63 - 1024, 2 ** 63, -(2 ** 63), 2 ** 63 + 2048,
    -(2 ** 63 + 2048), 2 ** 64, 2 ** 84, 2 ** 85, Number.MAX_VALUE, Number.MIN_VALUE, Infinity, -Infinity, NaN,
];

// Every array type gets functions of its own, so that each store stays monomorphic.
function makeFunctions(name, literal)
{
    return eval(`// ${name} ${literal}
    ({
        invariant(array, value, count) {
            for (let i = 0; i < count; i++)
                array[i & 7] = value;
        },
        literal(array, count) {
            for (let i = 0; i < count; i++)
                array[i & 7] = ${literal};
        },
        varying(array, source, count) {
            for (let i = 0; i < count; i++)
                array[i & 7] = source[i & 63];
        },
        underBranch(array, value, count, skip) {
            for (let i = 0; i < count; i++) {
                if (!skip)
                    array[i & 7] = value;
            }
        },
        pastTheEnd(array, value, count) {
            for (let i = 0; i < count; i++)
                array[i] = value;
        },
        atIndex(array, value, count, index) {
            for (let i = 0; i < count; i++)
                array[index] = value;
        },
    })`);
}

function checkAll(array, expected, message)
{
    for (let i = 0; i < array.length; i++)
        shouldBe(array[i], expected, `${message} [${i}]`);
}

const source = new Float64Array(64);
for (let i = 0; i < source.length; i++)
    source[i] = doubles[i % doubles.length];

for (const type of intTypes) {
    const functions = makeFunctions(type.name, "1.5");
    const array = new type(8);

    for (let round = 0; round < 60; round++) {
        for (const value of doubles) {
            const expected = reference(type, value);

            array.fill(1);
            functions.invariant(array, value, 100);
            checkAll(array, expected, `${type.name} invariant ${value}`);

            array.fill(1);
            functions.underBranch(array, value, 100, true);
            checkAll(array, 1, `${type.name} skipped ${value}`);

            // The array mode becomes out of bounds, where the stores past the end do nothing.
            array.fill(1);
            functions.pastTheEnd(array, value, 100);
            checkAll(array, expected, `${type.name} pastTheEnd ${value}`);

            array.fill(1);
            functions.atIndex(array, value, 100, 3);
            shouldBe(array[3], expected, `${type.name} atIndex ${value}`);
            shouldBe(array[4], 1, `${type.name} atIndex neighbour ${value}`);
        }

        functions.varying(array, source, 64);
        for (let i = 0; i < 8; i++)
            shouldBe(array[i], reference(type, source[56 + i]), `${type.name} varying [${i}]`);
    }

    for (const value of doubles) {
        array.fill(1);
        functions.underBranch(array, value, 100, false);
        checkAll(array, reference(type, value), `${type.name} not skipped ${value}`);

        // atIndex() only ever was in bounds: the exit comes after the conversion left the loop.
        array.fill(1);
        functions.atIndex(array, value, 100, 8);
        checkAll(array, 1, `${type.name} atIndex out of bounds ${value}`);
    }
}

for (const type of intTypes) {
    for (const literal of ["1.5", "-1.5", "300.5", "4294967295.5", "1e300", "-(2 ** 63 + 2048)", "NaN", "Infinity"]) {
        const functions = makeFunctions(type.name, literal);
        const array = new type(8);
        for (let round = 0; round < 1500; round++)
            functions.literal(array, 100);
        checkAll(array, reference(type, eval(literal)), `${type.name} literal ${literal}`);
    }
}

// One double stored into arrays of two types.
{
    function twoArrays(a, b, source, count)
    {
        for (let i = 0; i < count; i++) {
            const value = source[i & 63];
            a[i & 63] = value;
            b[i & 63] = value;
        }
    }
    noInline(twoArrays);

    const a = new Int8Array(64);
    const b = new Uint32Array(64);
    for (let round = 0; round < 3000; round++)
        twoArrays(a, b, source, 64);
    for (let i = 0; i < 64; i++) {
        shouldBe(a[i], reference(Int8Array, source[i]), `twoArrays Int8Array [${i}]`);
        shouldBe(b[i], reference(Uint32Array, source[i]), `twoArrays Uint32Array [${i}]`);
    }
}

// These do not convert through ToInt32.
{
    const clamped = makeFunctions("Uint8ClampedArray", "1.5");
    const array = new Uint8ClampedArray(8);
    const cases = [[0.5, 0], [1.5, 2], [2.5, 2], [254.5, 254], [254.6, 255], [300.7, 255], [-5.5, 0], [1e300, 255], [-1e300, 0], [Infinity, 255], [-Infinity, 0], [NaN, 0]];
    for (let round = 0; round < 200; round++) {
        for (const [value, expected] of cases) {
            array.fill(1);
            clamped.invariant(array, value, 100);
            checkAll(array, expected, `Uint8ClampedArray invariant ${value}`);
        }
        clamped.literal(array, 100);
        checkAll(array, 2, "Uint8ClampedArray literal 1.5");
    }

    for (const type of [Float32Array, Float64Array]) {
        const functions = makeFunctions(type.name, "1.5");
        const array = new type(8);
        for (let round = 0; round < 200; round++) {
            for (const value of [0.5, -1.5, 4294967295.5, Infinity, NaN, -0]) {
                functions.invariant(array, value, 100);
                checkAll(array, type === Float32Array ? Math.fround(value) : value, `${type.name} invariant ${value}`);
            }
            functions.literal(array, 100);
            checkAll(array, 1.5, `${type.name} literal 1.5`);
        }
    }
}

// After the functions have only seen doubles: everything else that a store converts.
{
    const functions = makeFunctions("Int32Array", "1.5");
    const array = new Int32Array(8);
    for (let round = 0; round < 3000; round++)
        functions.invariant(array, round + 0.5, 100);

    for (const [value, expected] of [[7, 7], [true, 1], [false, 0], [undefined, 0], [null, 0], ["12.9", 12], ["x", 0], [[], 0], [[5.5], 5]]) {
        array.fill(1);
        functions.invariant(array, value, 100);
        checkAll(array, expected, `Int32Array invariant ${typeof value} ${String(value)}`);
    }

    let calls = 0;
    const object = { valueOf() { return ++calls + 0.5; } };
    functions.invariant(array, object, 100);
    shouldBe(calls, 100, "valueOf() runs once for each store");
    for (let i = 0; i < 8; i++)
        shouldBe(array[i], (i < 4 ? 97 : 89) + i, `Int32Array invariant object [${i}]`);

    let threw = false;
    try {
        functions.invariant(array, 1n, 100);
    } catch (error) {
        threw = error instanceof TypeError;
    }
    shouldBe(threw, true, "a BigInt throws");
}

// Doubles and booleans from the start.
{
    const functions = makeFunctions("Int16Array booleans", "1.5");
    const array = new Int16Array(8);
    for (let round = 0; round < 3000; round++) {
        const value = round % 3 ? round + 0.5 : !(round % 2);
        functions.invariant(array, value, 100);
        checkAll(array, reference(Int16Array, Number(value)), `Int16Array invariant ${value}`);
    }
}

// A length-tracking array that shrinks under the loop's feet.
{
    function storeAndShrink(array, buffer, value, count, shrinkAt)
    {
        for (let i = 0; i < count; i++) {
            if (i === shrinkAt)
                buffer.resize(4 * 4);
            array[i & 7] = value;
        }
    }
    noInline(storeAndShrink);

    for (let round = 0; round < 3000; round++) {
        const buffer = new ArrayBuffer(8 * 4, { maxByteLength: 8 * 4 });
        const array = new Int32Array(buffer);
        const shrink = round % 500 === 499;
        storeAndShrink(array, buffer, round + 0.5, 100, shrink ? 50 : -1);
        shouldBe(array.length, shrink ? 4 : 8, "length");
        checkAll(array, round, `resizable ${round}`);
    }
}
