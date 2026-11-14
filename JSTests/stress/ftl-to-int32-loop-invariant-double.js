function shouldBe(actual, expected, name)
{
    if (actual !== expected)
        throw new Error(`bad value for ${name}: ${actual}, expected ${expected}`);
}

// Each loop converts a double to int32 that is the same in every iteration, in a way that the DFG
// does not see. Either `zero` is not a constant to it, or the conversion is part of a typed array
// store. B3 then finds that every input of the conversion is loop-invariant and hoists all of it
// that has no effects to the loop pre-header. That includes the call on the slow path of the
// conversion, which then runs before the branch that guards it, with whatever the double is.

const zeros = [
    "Math.imul(0, i)",
    "Math.imul(false, i)",
    "(i * 0) | 0",
    "i - i",
];

// [expression, its value when zero is 0]
const conversions = [
    // Doubles that the fast path converts.
    ["(zero + 0.5) | 0", 0],
    ["(zero + 5.5) | 0", 5],
    ["(zero - 5.5) | 0", -5],
    ["(zero + 5.5) >>> 0", 5],
    ["(zero * 1.5) >> 0", 0],
    ["(zero ** 2) >> 0", 0],
    ["Math.pow(zero, 2) | 0", 0],
    ["~(zero ** 2)", -1],
    ["(zero + 2147483648.5) | 0", -2147483648],
    ["(zero - 4294967297.5) | 0", -1],
    ["(zero + 2 ** 62 + 1024) | 0", 1024],
    // Doubles that only the slow path converts.
    ["(zero + 2 ** 63) | 0", 0],
    ["(zero + 2 ** 63 + 2048) | 0", 2048],
    ["(zero - 2 ** 63 - 2048) | 0", -2048],
    ["(zero + 2 ** 64 + 4096) | 0", 4096],
    ["(zero / zero) | 0", 0],
    ["(1 / zero) | 0", 0],
];

function testConversion(zero, [conversion, expected])
{
    const sumInLoop = new Function("count", `
        let sum = 0;
        for (let i = 0; i < count; ++i) {
            const zero = ${zero};
            sum = (sum + (${conversion})) | 0;
        }
        return sum;
    `);
    shouldBe(sumInLoop(testLoopCount), (expected * testLoopCount) | 0, `${conversion} with zero = ${zero}`);
}

for (const conversion of conversions)
    testConversion(zeros[0], conversion);

for (const zero of zeros.slice(1)) {
    testConversion(zero, conversions[0]);
    testConversion(zero, conversions[1]);
}

// [typed array, double to store, the element after the store]
const stores = [
    // Doubles that the fast path converts.
    [Int8Array, "1.5", 1],
    [Uint8Array, "255.5", 255],
    [Int16Array, "-32768.5", -32768],
    [Uint16Array, "65536.5", 0],
    [Int32Array, "2147483648.5", -2147483648],
    [Uint32Array, "-1.5", 4294967295],
    // Doubles that only the slow path converts.
    [Int32Array, "2 ** 63 + 2048", 2048],
    [Uint32Array, "-(2 ** 63 + 2048)", 4294965248],
    [Int32Array, "NaN", 0],
];

function testStore([TypedArray, double, expected])
{
    const storeInLoop = new Function("array", "count", `
        for (let i = 0; i < count; ++i)
            array[i & 7] = ${double};
    `);
    const array = new TypedArray(8);
    storeInLoop(array, testLoopCount);
    for (let i = 0; i < array.length; ++i)
        shouldBe(array[i], expected, `${TypedArray.name}[${i}] = ${double}`);
}

for (const store of stores)
    testStore(store);
