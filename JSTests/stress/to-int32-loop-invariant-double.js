function shouldBe(actual, expected, input)
{
    if (actual !== expected)
        throw new Error(`bad value for ${input}: ${actual}, expected ${expected}`);
}

// Reference ToInt32 through BigInt, independent of the conversion under test.
function toInt32Reference(x)
{
    if (!Number.isFinite(x))
        return 0;
    return Number(BigInt.asIntN(32, BigInt(Math.trunc(x))));
}

// The stored double does not change within the loop, so B3 hoists the slow path call of its
// conversion out of the loop, where the call runs whether or not the fast path fails.
function storeInLoop(array, value)
{
    for (let i = 0; i < 50; i++)
        array[0] = value;
    return array[0];
}
noInline(storeInLoop);

function storeInLoopUnderBranch(array, value, skip)
{
    for (let i = 0; i < 50; i++) {
        if (!skip)
            array[0] = value;
    }
    return array[0];
}
noInline(storeInLoopUnderBranch);

function atomicsStoreInLoop(array, value)
{
    for (let i = 0; i < 50; i++)
        Atomics.store(array, 0, value);
    return array[0];
}
noInline(atomicsStoreInLoop);

function storeConstantInLoop(array)
{
    for (let i = 0; i < 50; i++)
        array[0] = 1.5;
    return array[0];
}
noInline(storeConstantInLoop);

// Both sides of 2^63, where the x86_64 fast path starts to fail.
const inputs = [
    0.5, -0.5, 1.5, -1.5, 301000.5, 2147483647.9, -2147483648.9, 4294967295.5,
    2 ** 52 + 1, 2 ** 62, -(2 ** 62), 2 ** 63 - 1024, 2 ** 63, -(2 ** 63),
    2 ** 63 + 2048, -(2 ** 63 + 2048), 2 ** 64, 2 ** 84, 2 ** 85,
    Number.MAX_VALUE, Infinity, -Infinity, NaN,
];
const expected = inputs.map(toInt32Reference);

const int32Array = new Int32Array(1);

for (let i = 0; i < testLoopCount; i++) {
    const input = inputs[i % inputs.length];
    const result = expected[i % inputs.length];

    shouldBe(storeInLoop(int32Array, input), result, input);
    shouldBe(atomicsStoreInLoop(int32Array, input), result, input);

    int32Array[0] = 42;
    shouldBe(storeInLoopUnderBranch(int32Array, input, true), 42, input);

    shouldBe(storeConstantInLoop(int32Array), 1, 1.5);
}

for (let i = 0; i < inputs.length; i++)
    shouldBe(storeInLoopUnderBranch(int32Array, inputs[i], false), expected[i], inputs[i]);
