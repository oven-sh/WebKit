// %TypedArray%.prototype.indexOf / lastIndexOf compare with IsStrictlyEqual and includes with SameValueZero,
// against the element values as Numbers / BigInts. A needle the element type cannot represent exactly
// matches nothing, whatever its representation (int32, double, BigInt). It must not be wrapped, clamped
// or rounded into a value that does exist in the array.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + expected + " but got " + actual);
}

function expectFound(array, needle, index, description) {
    shouldBe(array.indexOf(needle), index, description + " indexOf");
    shouldBe(array.lastIndexOf(needle), index, description + " lastIndexOf");
    shouldBe(array.includes(needle), true, description + " includes");
}

function expectMissing(array, needle, description) {
    shouldBe(array.indexOf(needle), -1, description + " indexOf");
    shouldBe(array.lastIndexOf(needle), -1, description + " lastIndexOf");
    shouldBe(array.includes(needle), false, description + " includes");
}

// A Float64Array element read always produces a double-encoded JSValue, even for an integral value that
// would otherwise be stored as an int32. This is how we get e.g. -1 and 256 onto the double path.
const doubleBox = new Float64Array(1);
function asDouble(number) {
    doubleBox[0] = number;
    return doubleBox[0];
}

const bitsBox = new BigUint64Array(doubleBox.buffer);
function nextDoubleUp(number) {
    doubleBox[0] = number;
    bitsBox[0] = bitsBox[0] + 1n;
    return doubleBox[0];
}

function testUint8Clamped() {
    const array = new Uint8ClampedArray([7, 0, 255]);
    const name = "Uint8ClampedArray([7, 0, 255])";

    expectFound(array, 7, 0, name + " int32 7");
    expectFound(array, 0, 1, name + " int32 0");
    expectFound(array, 255, 2, name + " int32 255");
    expectFound(array, asDouble(7), 0, name + " double 7");
    expectFound(array, asDouble(0), 1, name + " double 0");
    expectFound(array, -0, 1, name + " -0");
    expectFound(array, asDouble(255), 2, name + " double 255");

    for (const needle of [-1, 256, 300, 511, 263, -249, 0x7fffffff, -0x80000000])
        expectMissing(array, needle, name + " int32 " + needle);

    // Every one of these used to match: the needle was reduced to the low byte of cvttsd2si(needle).
    for (const needle of [-1, 256, 263, -249, 1e10, -1e10, 2 ** 31, -(2 ** 31), 2 ** 31 - 1, 2 ** 32, 2 ** 32 + 7, 2 ** 53, -(2 ** 53), 2 ** 64, Number.MAX_VALUE, Infinity, -Infinity])
        expectMissing(array, asDouble(needle), name + " double " + needle);

    for (const needle of [0.5, -0.5, 7.5, 255.5, 254.99999999999997, Number.MIN_VALUE, -Number.MIN_VALUE])
        expectMissing(array, needle, name + " fractional " + needle);

    shouldBe(array.indexOf(NaN), -1, name + " indexOf NaN");
    shouldBe(array.lastIndexOf(NaN), -1, name + " lastIndexOf NaN");
    shouldBe(array.includes(NaN), false, name + " includes NaN");

    expectMissing(array, 7n, name + " BigInt 7n");
    expectMissing(array, "7", name + " string '7'");
}

function testIntegral() {
    const cases = [
        [Int8Array, [7, 0, -1, -128, 127], [[255, -1], [128, -1], [-129, -1], [383, -1], [2 ** 31 - 1, -1], [-(2 ** 31), -1]]],
        [Uint8Array, [7, 0, 255], [[-1, -1], [256, -1], [511, -1], [-249, -1], [2 ** 31 - 1, -1]]],
        [Int16Array, [7, 0, -1, -32768, 32767], [[65535, -1], [32768, -1], [-32769, -1], [65543, -1]]],
        [Uint16Array, [7, 0, 65535], [[-1, -1], [65536, -1], [65543, -1], [-65529, -1]]],
        [Int32Array, [7, 0, -1, -(2 ** 31), 2 ** 31 - 1], [[2 ** 31, -1], [2 ** 32 - 1, -1], [-(2 ** 31) - 1, -1], [2 ** 32 + 7, -1]]],
        [Uint32Array, [7, 0, 2 ** 32 - 1, 2 ** 31], [[-1, -1], [2 ** 32, -1], [2 ** 32 + 7, -1], [-(2 ** 31), -1]]],
    ];
    for (const [constructor, elements, missing] of cases) {
        const array = new constructor(elements);
        const name = constructor.name + "(" + elements + ")";
        elements.forEach((element, index) => {
            expectFound(array, element, index, name + " int32-or-double " + element);
            expectFound(array, asDouble(element), index, name + " double " + element);
        });
        for (const [needle] of missing) {
            expectMissing(array, needle, name + " " + needle);
            expectMissing(array, asDouble(needle), name + " double " + needle);
        }
        for (const needle of [1e10, -1e10, 2 ** 53, -(2 ** 53), 2 ** 63, 2 ** 64, -(2 ** 64), Number.MAX_VALUE, Infinity, -Infinity, 0.5, -0.5, 7.5])
            expectMissing(array, asDouble(needle), name + " double " + needle);
        expectFound(array, -0, 1, name + " -0");
        shouldBe(array.includes(NaN), false, name + " includes NaN");
    }
}

function testFloat32() {
    const array = new Float32Array([16777216, 0.5, -0, 2 ** 31, Infinity, -Infinity, 0.1, 3.4028234663852886e38]);
    const name = "Float32Array";

    expectFound(array, 16777216, 0, name + " int32 2^24");
    expectFound(array, asDouble(16777216), 0, name + " double 2^24");
    // 2^24 + 1 is the first int32 a float cannot hold. It used to be rounded to 2^24 and "found".
    expectMissing(array, 16777217, name + " int32 2^24 + 1");
    expectMissing(array, asDouble(16777217), name + " double 2^24 + 1");
    expectMissing(array, 16777215 + 2, name + " computed int32 2^24 + 1");
    // INT32_MAX rounds to 2^31 as a float.
    expectMissing(array, 2147483647, name + " int32 INT32_MAX");
    expectMissing(array, asDouble(2147483647), name + " double INT32_MAX");
    expectFound(array, asDouble(2 ** 31), 3, name + " double 2^31");

    expectFound(array, 0.5, 1, name + " 0.5");
    expectFound(array, 0, 2, name + " int32 0 matches -0");
    expectFound(array, -0, 2, name + " -0");
    expectFound(array, asDouble(0), 2, name + " double 0");
    expectFound(array, Infinity, 4, name + " Infinity");
    expectFound(array, -Infinity, 5, name + " -Infinity");
    // The element is Math.fround(0.1), which is not the double 0.1.
    expectMissing(array, 0.1, name + " 0.1");
    expectFound(array, Math.fround(0.1), 6, name + " Math.fround(0.1)");
    const floatMax = 3.4028234663852886e38;
    shouldBe(Math.fround(floatMax), floatMax, "FLT_MAX literal");
    expectFound(array, floatMax, 7, name + " FLT_MAX");
    // Beyond FLT_MAX: must not be converted to float at all (that conversion is undefined behavior in C++).
    expectMissing(array, nextDoubleUp(floatMax), name + " the double after FLT_MAX");
    expectMissing(array, 1e300, name + " 1e300");
    expectMissing(array, -1e300, name + " -1e300");
    expectMissing(array, Number.MAX_VALUE, name + " Number.MAX_VALUE");

    const withNaN = new Float32Array([1, NaN, 3]);
    shouldBe(withNaN.indexOf(NaN), -1, name + " indexOf NaN");
    shouldBe(withNaN.lastIndexOf(NaN), -1, name + " lastIndexOf NaN");
    shouldBe(withNaN.includes(NaN), true, name + " includes NaN");
}

function testFloat16() {
    const array = new Float16Array([2048, 0.5, -0, 65504, Infinity, -Infinity, 0.1, -65504]);
    const name = "Float16Array";

    expectFound(array, 2048, 0, name + " int32 2048");
    expectFound(array, asDouble(2048), 0, name + " double 2048");
    // 2^11 + 1 is the first int32 a Float16 cannot hold. It used to be rounded to 2048 and "found".
    expectMissing(array, 2049, name + " int32 2049");
    expectMissing(array, asDouble(2049), name + " double 2049");
    expectMissing(array, 2047 + 2, name + " computed int32 2049");
    expectFound(array, 65504, 3, name + " int32 65504 (max finite)");
    expectFound(array, -65504, 7, name + " int32 -65504");
    // These int32 needles used to become +/-Infinity and match the Infinity elements.
    expectMissing(array, 65536, name + " int32 65536");
    expectMissing(array, -65536, name + " int32 -65536");
    expectMissing(array, 65520, name + " int32 65520");
    expectMissing(array, 2147483647, name + " int32 INT32_MAX");
    expectMissing(array, -2147483648, name + " int32 INT32_MIN");
    expectMissing(array, asDouble(65536), name + " double 65536");
    expectMissing(array, asDouble(65520), name + " double 65520");
    expectMissing(array, 1e300, name + " 1e300");

    expectFound(array, 0.5, 1, name + " 0.5");
    expectFound(array, 0, 2, name + " int32 0 matches -0");
    expectFound(array, -0, 2, name + " -0");
    expectFound(array, Infinity, 4, name + " Infinity");
    expectFound(array, -Infinity, 5, name + " -Infinity");
    expectMissing(array, 0.1, name + " 0.1");
    expectFound(array, array[6], 6, name + " f16round(0.1)");

    const withNaN = new Float16Array([1, NaN, 3]);
    shouldBe(withNaN.indexOf(NaN), -1, name + " indexOf NaN");
    shouldBe(withNaN.includes(NaN), true, name + " includes NaN");
}

function testFloat64() {
    const array = new Float64Array([16777217, 2147483647, -0, 2 ** 53, 0.1]);
    const name = "Float64Array";
    expectFound(array, 16777217, 0, name + " int32 16777217");
    expectFound(array, 2147483647, 1, name + " int32 INT32_MAX");
    expectFound(array, asDouble(2147483647), 1, name + " double INT32_MAX");
    expectFound(array, 0, 2, name + " 0 matches -0");
    expectFound(array, 2 ** 53, 3, name + " 2^53");
    expectFound(array, 0.1, 4, name + " 0.1");
    expectMissing(array, 2 ** 53 + 2, name + " 2^53 + 2");
    expectMissing(array, Infinity, name + " Infinity");
}

function testBigInt64() {
    const array = new BigInt64Array([1n, 0n, -1n, -(2n ** 63n), 2n ** 63n - 1n]);
    const name = "BigInt64Array";

    expectFound(array, 1n, 0, name + " 1n");
    expectFound(array, 0n, 1, name + " 0n");
    expectFound(array, -1n, 2, name + " -1n");
    expectFound(array, -(2n ** 63n), 3, name + " INT64_MIN");
    expectFound(array, 2n ** 63n - 1n, 4, name + " INT64_MAX");

    // Each of these used to be reduced modulo 2^64 into one of the elements above.
    expectMissing(array, 2n ** 64n + 1n, name + " 2^64 + 1");
    expectMissing(array, 2n ** 64n, name + " 2^64");
    expectMissing(array, -(2n ** 64n), name + " -2^64");
    expectMissing(array, 2n ** 64n - 1n, name + " 2^64 - 1");
    expectMissing(array, 2n ** 63n, name + " 2^63");
    expectMissing(array, -(2n ** 63n) - 1n, name + " INT64_MIN - 1");
    expectMissing(array, 2n ** 128n + 1n, name + " 2^128 + 1");
    expectMissing(array, -(2n ** 128n) - 1n, name + " -2^128 - 1");
    expectMissing(array, (1n << 200n) - 1n, name + " 2^200 - 1");

    expectMissing(array, 1, name + " Number 1");
    expectMissing(array, -1, name + " Number -1");
    expectMissing(array, "1", name + " string '1'");
}

function testBigUint64() {
    const array = new BigUint64Array([1n, 0n, 2n ** 64n - 1n, 2n ** 63n]);
    const name = "BigUint64Array";

    expectFound(array, 1n, 0, name + " 1n");
    expectFound(array, 0n, 1, name + " 0n");
    expectFound(array, 2n ** 64n - 1n, 2, name + " UINT64_MAX");
    expectFound(array, 2n ** 63n, 3, name + " 2^63");

    // Each of these used to be reduced modulo 2^64 into one of the elements above.
    expectMissing(array, -1n, name + " -1n");
    expectMissing(array, -(2n ** 63n), name + " -2^63");
    expectMissing(array, 2n ** 64n, name + " 2^64");
    expectMissing(array, 2n ** 64n + 1n, name + " 2^64 + 1");
    expectMissing(array, -(2n ** 64n), name + " -2^64");
    expectMissing(array, -(2n ** 64n) + 1n, name + " -2^64 + 1");
    expectMissing(array, 2n ** 128n, name + " 2^128");
    expectMissing(array, (1n << 200n) + 1n, name + " 2^200 + 1");

    expectMissing(array, 1, name + " Number 1");
    expectMissing(array, 0, name + " Number 0");
}

// Also read needles out of a double-shaped JS array, the way the original report did.
function testDoubleArraySource() {
    const array = new Uint8ClampedArray([7, 0, 255]);
    const needles = [1e10, -1e10, 2 ** 31, -(2 ** 31), Infinity, -Infinity, 2 ** 53, 2147483647, 256, -1, 300, NaN, 0.5];
    for (let i = 0; i < needles.length; ++i) {
        shouldBe(array.indexOf(needles[i]), -1, "double array source indexOf " + needles[i]);
        shouldBe(array.lastIndexOf(needles[i]), -1, "double array source lastIndexOf " + needles[i]);
        shouldBe(array.includes(needles[i]), false, "double array source includes " + needles[i]);
    }
}

for (let i = 0; i < 200; ++i) {
    testUint8Clamped();
    testIntegral();
    testFloat32();
    testFloat16();
    testFloat64();
    testBigInt64();
    testBigUint64();
    testDoubleArraySource();
}
