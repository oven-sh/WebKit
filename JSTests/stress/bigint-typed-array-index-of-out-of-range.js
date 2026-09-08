function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + expected + " but got " + actual);
}

// %TypedArray%.prototype.indexOf / lastIndexOf compare with IsStrictlyEqual and includes with SameValueZero
// against the element's BigInt value. A BigInt that is not representable in the element type can never match,
// even when it is congruent to an element modulo 2^64.

function test(array, needle, expectedIndex) {
    let name = array.constructor.name + "(" + array.length + ") needle " + needle + "n";
    shouldBe(array.indexOf(needle), expectedIndex, name + " indexOf");
    shouldBe(array.lastIndexOf(needle), expectedIndex, name + " lastIndexOf");
    shouldBe(array.includes(needle), expectedIndex !== -1, name + " includes");
    shouldBe(array.indexOf(needle, 1), expectedIndex >= 1 ? expectedIndex : -1, name + " indexOf fromIndex 1");
    shouldBe(array.lastIndexOf(needle, -2), expectedIndex <= array.length - 2 ? expectedIndex : -1, name + " lastIndexOf fromIndex -2");
    shouldBe(array.includes(needle, -1), expectedIndex === array.length - 1, name + " includes fromIndex -1");
    shouldBe(Array.prototype.indexOf.call(array, needle), expectedIndex, name + " Array.prototype.indexOf");
    shouldBe(Array.prototype.includes.call(array, needle), expectedIndex !== -1, name + " Array.prototype.includes");
}

const int64Max = 2n ** 63n - 1n;
const int64Min = -(2n ** 63n);
const uint64Max = 2n ** 64n - 1n;

const int64Cases = [
    // [elements, needle, expected index]
    [[3n, -1n, 7n], 2n ** 64n - 1n, -1], // wraps to -1n
    [[3n, -1n, 7n], 2n ** 64n + 3n, -1], // wraps to 3n
    [[3n, -1n, 7n], -(2n ** 64n) + 7n, -1], // wraps to 7n
    [[3n, 0n, 7n], 2n ** 64n, -1], // wraps to 0n
    [[3n, 0n, 7n], -(2n ** 64n), -1], // wraps to 0n
    [[3n, 0n, 7n], 2n ** 128n, -1], // wraps to 0n
    [[3n, int64Min, 7n], 2n ** 63n, -1], // wraps to -2^63
    [[3n, int64Max, 7n], int64Min - 1n, -1], // wraps to 2^63 - 1
    [[3n, 5n, 7n], 2n ** 200n + 5n, -1], // many digits, low 64 bits are 5
    [[3n, -5n, 7n], -(2n ** 200n) - 5n, -1],
    // Representable needles still match, including the boundaries.
    [[3n, -1n, 7n], -1n, 1],
    [[3n, -1n, 7n], 3n, 0],
    [[3n, -1n, 7n], 7n, 2],
    [[3n, 0n, 7n], 0n, 1],
    [[3n, 0n, 7n], -0n, 1],
    [[3n, int64Max, 7n], int64Max, 1],
    [[3n, int64Min, 7n], int64Min, 1],
    [[3n, 2n ** 32n, 7n], 2n ** 32n, 1],
    [[3n, -(2n ** 32n), 7n], -(2n ** 32n), 1],
    [[3n, -1n, 7n], 4n, -1],
];

const uint64Cases = [
    [[3n, uint64Max, 7n], -1n, -1], // wraps to 2^64 - 1
    [[3n, int64Max, 7n], int64Min - 1n, -1], // wraps to 2^63 - 1
    [[3n, 2n ** 63n, 7n], int64Min, -1], // wraps to 2^63
    [[3n, 0n, 7n], 2n ** 64n, -1], // wraps to 0n
    [[3n, 0n, 7n], -(2n ** 64n), -1], // wraps to 0n
    [[3n, 0n, 7n], 2n ** 128n, -1], // wraps to 0n
    [[3n, 5n, 7n], 2n ** 64n + 5n, -1], // wraps to 5n
    [[3n, 5n, 7n], -(2n ** 64n) + 5n, -1], // wraps to 5n
    [[3n, 5n, 7n], 2n ** 200n + 5n, -1],
    [[3n, uint64Max - 4n, 7n], -5n, -1], // wraps to 2^64 - 5
    // Representable needles still match, including the boundaries.
    [[3n, uint64Max, 7n], uint64Max, 1],
    [[3n, 0n, 7n], 0n, 1],
    [[3n, 0n, 7n], -0n, 1],
    [[3n, 2n ** 63n, 7n], 2n ** 63n, 1],
    [[3n, int64Max, 7n], int64Max, 1],
    [[3n, 2n ** 32n, 7n], 2n ** 32n, 1],
    [[3n, 5n, 7n], 3n, 0],
    [[3n, 5n, 7n], 7n, 2],
    [[3n, 5n, 7n], 4n, -1],
];

function makeViews(constructor, elements) {
    let views = [];

    views.push(new constructor(elements));

    let padded = new constructor(elements.length + 2);
    padded.set(elements, 1);
    views.push(padded.subarray(1, elements.length + 1));

    let resizable = new ArrayBuffer(elements.length * 8, { maxByteLength: elements.length * 8 + 64 });
    let tracking = new constructor(resizable);
    tracking.set(elements);
    views.push(tracking);

    let fixedWindow = new constructor(resizable, 0, elements.length);
    views.push(fixedWindow);

    let shared = new constructor(new SharedArrayBuffer(elements.length * 8));
    shared.set(elements);
    views.push(shared);

    return views;
}

function run() {
    for (let [elements, needle, expected] of int64Cases) {
        for (let view of makeViews(BigInt64Array, elements))
            test(view, needle, expected);
    }
    for (let [elements, needle, expected] of uint64Cases) {
        for (let view of makeViews(BigUint64Array, elements))
            test(view, needle, expected);
    }
}

for (let i = 0; i < 200; ++i)
    run();

// A non-BigInt needle never matches a BigInt element, and a BigInt needle never matches a Number element.
{
    let i64 = new BigInt64Array([0n, 1n, -1n]);
    shouldBe(i64.indexOf(1), -1, "BigInt64Array indexOf Number");
    shouldBe(i64.includes(-1), false, "BigInt64Array includes Number");
    shouldBe(i64.lastIndexOf("1"), -1, "BigInt64Array lastIndexOf String");
    let u64 = new BigUint64Array([0n, 1n, uint64Max]);
    shouldBe(u64.indexOf(2 ** 64), -1, "BigUint64Array indexOf Number 2^64");
    shouldBe(u64.includes(18446744073709551615), false, "BigUint64Array includes Number 2^64-1");
    let f64 = new Float64Array([0, 1, 2 ** 64]);
    shouldBe(f64.indexOf(2n ** 64n), -1, "Float64Array indexOf BigInt");
    shouldBe(f64.includes(1n), false, "Float64Array includes BigInt");
}
