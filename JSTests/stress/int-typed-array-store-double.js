function shouldBe(actual, expected, what, detail = "")
{
    if (!Object.is(actual, expected))
        throw new Error(`${what} ${String(detail)}: got ${actual}, expected ${expected}`);
}

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
    0.5, -0.5, 1.5, -1.5, 127.9, 128.5, -128.9, -129.5, 255.5, 256.5, 32767.5, 32768.5, -32768.9, -32769.5,
    65535.5, 65536.5, 301000.5, 2147483647.9, 2147483648.5, -2147483648.9, -2147483649.5, 4294967295.5,
    4294967296.5, 2 ** 52 + 1, 2 ** 53 + 2, 2 ** 62, -(2 ** 62), 2 ** 63 - 1024, 2 ** 63, -(2 ** 63),
    2 ** 63 + 2048, -(2 ** 63 + 2048), 2 ** 64, 2 ** 84, 2 ** 85, Number.MAX_VALUE, Number.MIN_VALUE,
    Infinity, -Infinity, NaN,
];

// Every caller gets functions of its own, so that each store sees one type of array.
function makeFunctions(name, literal = "1.5")
{
    return eval(`// ${name} ${literal}
    ({
        invariant(array, value) {
            for (let i = 0; i < 16; i++)
                array[i & 7] = value;
        },
        literal(array) {
            for (let i = 0; i < 16; i++)
                array[i & 7] = ${literal};
        },
        varying(array, source, offset) {
            for (let i = 0; i < 16; i++)
                array[i & 7] = source[offset + i];
        },
        readThenStore(array, value) {
            let sum = 0;
            for (let i = 0; i < 16; i++) {
                sum += array[i & 7];
                array[i & 7] = value;
            }
            return sum;
        },
        underBranch(array, value, skip) {
            for (let i = 0; i < 16; i++) {
                if (!skip)
                    array[i & 7] = value;
            }
        },
        pastTheEnd(array, value) {
            for (let i = 0; i < 16; i++)
                array[i] = value;
        },
        atIndex(array, value, index) {
            for (let i = 0; i < 16; i++)
                array[index] = value;
        },
    })`);
}

function checkAll(array, expected, what, detail)
{
    for (let i = 0; i < array.length; i++)
        shouldBe(array[i], expected, what, detail);
}

const source = new Float64Array(doubles.length + 16);
for (let i = 0; i < source.length; i++)
    source[i] = doubles[i % doubles.length];

for (const type of [Int8Array, Uint8Array, Int16Array, Uint16Array, Int32Array, Uint32Array]) {
    const functions = makeFunctions(type.name);
    const array = new type(8);
    const expected = doubles.map(value => reference(type, value));

    for (let i = 0; i < testLoopCount; i++) {
        const which = i % doubles.length;

        array[i & 7] = 1;
        functions.invariant(array, doubles[which]);
        shouldBe(array[i & 7], expected[which], type.name + " invariant", doubles[which]);

        array.fill(1);
        shouldBe(functions.readThenStore(array, doubles[which]), 8 + 8 * expected[which], type.name + " readThenStore sum", doubles[which]);
        shouldBe(array[i & 7], expected[which], type.name + " readThenStore", doubles[which]);
    }
}

// The rest does not depend on the width of an element.
{
    const functions = makeFunctions("Int32Array shapes");
    const array = new Int32Array(8);
    const expected = doubles.map(value => reference(Int32Array, value));

    for (let i = 0; i < testLoopCount; i++) {
        const which = i % doubles.length;

        functions.varying(array, source, which);
        shouldBe(array[i & 7], expected[(which + 8 + (i & 7)) % doubles.length], "varying from", which);

        array[i & 7] = 1;
        functions.underBranch(array, doubles[which], true);
        shouldBe(array[i & 7], 1, "underBranch skipped", doubles[which]);

        // The array mode becomes out of bounds, where the stores past the end do nothing.
        functions.pastTheEnd(array, doubles[which]);
        shouldBe(array[i & 7], expected[which], "pastTheEnd", doubles[which]);

        array[4] = 1;
        functions.atIndex(array, doubles[which], 3);
        shouldBe(array[3], expected[which], "atIndex", doubles[which]);
        shouldBe(array[4], 1, "atIndex neighbour", doubles[which]);
    }

    for (let which = 0; which < doubles.length; which++) {
        array.fill(1);
        functions.underBranch(array, doubles[which], false);
        checkAll(array, expected[which], "underBranch", doubles[which]);

        // atIndex() only ever was in bounds: the exit comes after the conversion left the loop.
        array.fill(1);
        functions.atIndex(array, doubles[which], 8);
        checkAll(array, 1, "atIndex out of bounds", doubles[which]);
    }
}

{
    const cases = [
        [Int8Array, "1.5"], [Uint8Array, "-1.5"], [Int16Array, "300.5"], [Uint16Array, "4294967295.5"],
        [Int32Array, "1e300"], [Uint32Array, "-(2 ** 63 + 2048)"], [Int32Array, "NaN"], [Uint8Array, "Infinity"],
    ].map(([type, literal]) => [makeFunctions(type.name, literal), new type(8), reference(type, eval(literal)), type.name + " literal " + literal]);

    for (let i = 0; i < testLoopCount; i++) {
        for (const [functions, array, expected, message] of cases) {
            array[i & 7] = 1;
            functions.literal(array);
            shouldBe(array[i & 7], expected, message);
        }
    }
}

// One double stored into arrays of two types.
{
    function twoArrays(a, b, source, offset)
    {
        for (let i = 0; i < 16; i++) {
            const value = source[offset + i];
            a[i] = value;
            b[i] = value;
        }
    }
    noInline(twoArrays);

    const a = new Int8Array(16);
    const b = new Uint32Array(16);
    for (let i = 0; i < testLoopCount; i++) {
        const which = i % doubles.length;
        twoArrays(a, b, source, which);
        shouldBe(a[i & 15], reference(Int8Array, source[which + (i & 15)]), "twoArrays Int8Array from", which);
        shouldBe(b[i & 15], reference(Uint32Array, source[which + (i & 15)]), "twoArrays Uint32Array from", which);
    }
}

// These do not convert through ToInt32.
{
    const clampedCases = [[0.5, 0], [1.5, 2], [2.5, 2], [254.5, 254], [254.6, 255], [300.7, 255], [-5.5, 0], [1e300, 255], [-1e300, 0], [Infinity, 255], [-Infinity, 0], [NaN, 0]];
    const floatCases = [0.5, -1.5, 4294967295.5, 1e300, Infinity, NaN, -0];
    const clamped = [makeFunctions("Uint8ClampedArray"), new Uint8ClampedArray(8)];
    const float32 = [makeFunctions("Float32Array"), new Float32Array(8)];
    const float64 = [makeFunctions("Float64Array"), new Float64Array(8)];

    for (let i = 0; i < testLoopCount; i++) {
        const [value, expected] = clampedCases[i % clampedCases.length];
        clamped[0].invariant(clamped[1], value);
        shouldBe(clamped[1][i & 7], expected, "Uint8ClampedArray invariant", value);
        clamped[0].readThenStore(clamped[1], value);
        shouldBe(clamped[1][i & 7], expected, "Uint8ClampedArray readThenStore", value);
        clamped[0].literal(clamped[1]);
        shouldBe(clamped[1][i & 7], 2, "Uint8ClampedArray literal 1.5");

        const float = floatCases[i % floatCases.length];
        float32[0].invariant(float32[1], float);
        shouldBe(float32[1][i & 7], Math.fround(float), "Float32Array invariant", float);
        float64[0].invariant(float64[1], float);
        shouldBe(float64[1][i & 7], float, "Float64Array invariant", float);
        float64[0].readThenStore(float64[1], float);
        shouldBe(float64[1][i & 7], float, "Float64Array readThenStore", float);
        float64[0].literal(float64[1]);
        shouldBe(float64[1][i & 7], 1.5, "Float64Array literal 1.5");
    }
}

// After the function has only seen doubles: everything else that a store converts.
{
    const functions = makeFunctions("Int32Array then other values");
    const array = new Int32Array(8);
    for (let i = 0; i < testLoopCount; i++)
        functions.invariant(array, i + 0.5);

    for (const [value, expected] of [[7, 7], [true, 1], [false, 0], [undefined, 0], [null, 0], ["12.9", 12], ["x", 0], [[], 0], [[5.5], 5]]) {
        array.fill(1);
        functions.invariant(array, value);
        checkAll(array, expected, "invariant " + typeof value, value);
    }

    let calls = 0;
    functions.invariant(array, { valueOf() { return ++calls + 0.5; } });
    shouldBe(calls, 16, "valueOf() runs once for each store");
    for (let i = 0; i < 8; i++)
        shouldBe(array[i], 9 + i, "invariant object at", i);

    let threw = false;
    try {
        functions.invariant(array, 1n);
    } catch (error) {
        threw = error instanceof TypeError;
    }
    shouldBe(threw, true, "a BigInt throws");
}

// Doubles and booleans from the start.
{
    const functions = makeFunctions("Int16Array with booleans");
    const array = new Int16Array(8);
    for (let i = 0; i < testLoopCount; i++) {
        const value = i % 3 ? i + 0.5 : !(i % 2);
        functions.invariant(array, value);
        shouldBe(array[i & 7], reference(Int16Array, Number(value)), "Int16Array invariant", value);
    }
}

// A length-tracking array, first always in bounds, then shrunk.
{
    const functions = makeFunctions("Int32Array length-tracking");
    const buffer = new ArrayBuffer(8 * 4, { maxByteLength: 8 * 4 });
    const array = new Int32Array(buffer);
    for (let i = 0; i < testLoopCount; i++) {
        functions.invariant(array, i + 0.5);
        shouldBe(array[i & 7], i, "length-tracking");
    }

    buffer.resize(4 * 4);
    functions.invariant(array, 1e10 + 0.5);
    shouldBe(array.length, 4, "length after resize()");
    checkAll(array, reference(Int32Array, 1e10), "length-tracking, shrunk");
}
