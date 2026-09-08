function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": got " + actual + ", expected " + expected);
}

// indexOf and lastIndexOf use IsStrictlyEqual and includes uses SameValueZero, on Number values.
// A needle that the element type cannot represent exactly is equal to no element. The result
// must not depend on whether the engine holds the needle as an int32 or as a double.

const scratch = new Float64Array(1);
function asDouble(value)
{
    scratch[0] = value;
    return scratch[0];
}

function checkOne(array, needle, expected, name)
{
    shouldBe(array.indexOf(needle), expected, name + " indexOf");
    shouldBe(array.lastIndexOf(needle), expected, name + " lastIndexOf");
    shouldBe(array.includes(needle), expected !== -1, name + " includes");
}

function check(array, needle, expected)
{
    const name = array.constructor.name + " " + needle;
    checkOne(array, needle, expected, name);
    checkOne(array, asDouble(needle), expected, name + " as double");
}

function test()
{
    {
        const array = new Float32Array([16777216, 2147483648, -16777216, -2147483648, 1]);
        shouldBe(array[1], 2147483648, "Float32Array holds 2^31");

        check(array, 16777216, 0);
        check(array, 16777217, -1); // rounds to 16777216 as a float
        check(array, 16777216.5 + 0.5, -1);
        check(array, parseInt("16777217"), -1);
        check(array, 2147483647, -1); // rounds to 2147483648 as a float
        check(array, 2147483648, 1);
        check(array, -16777216, 2);
        check(array, -16777217, -1);
        check(array, -2147483647, -1);
        check(array, -2147483648, 3);
        check(array, 1, 4);
    }

    {
        const array = new Float16Array([Infinity, 2048, 65504, -Infinity, -2048, 1]);

        check(array, Infinity, 0);
        check(array, 65520, -1); // rounds to Infinity as a half
        check(array, 65535, -1);
        check(array, 65536, -1);
        check(array, 2147483647, -1);
        check(array, 2048, 1);
        check(array, 2049, -1); // rounds to 2048 as a half
        check(array, 65504, 2);
        check(array, 65505, -1); // rounds to 65504 as a half
        check(array, 65519, -1);
        check(array, -Infinity, 3);
        check(array, -65520, -1);
        check(array, -2147483648, -1);
        check(array, -2048, 4);
        check(array, -2049, -1);
        check(array, 1, 5);
    }

    {
        // Every int32 is a double, so a Float64Array is unaffected.
        const array = new Float64Array([2147483647, -2147483648, 16777217]);
        check(array, 2147483647, 0);
        check(array, -2147483648, 1);
        check(array, 16777217, 2);
        check(array, 16777216, -1);
    }

    {
        const array = new Int32Array([2147483647, -2147483648, 16777217]);
        check(array, 2147483647, 0);
        check(array, -2147483648, 1);
        check(array, 16777217, 2);
        check(array, 16777216, -1);
    }
}

for (let i = 0; i < 1e3; ++i)
    test();
