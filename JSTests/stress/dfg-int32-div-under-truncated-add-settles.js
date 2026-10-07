//@ skip if not $jitTests
//@ $skipModes << :lockdown
//@ defaultNoEagerRun

// An Int32 division whose quotient goes through an add before it is truncated: `(x / y + 1) | 0`. The add does not
// tell the division that its result is used as a number, but the division is checked all the same (it is unchecked
// only right under the truncation), so it exits with Overflow on every quotient that has a remainder. The DFG used to
// read "not used as a number" as "may stay Int32 although it overflows", and made the same code in every compile.

// Each shape is two functions of different source text: the one under test, and a copy that never reaches the DFG.
function make(name, body) {
    return new Function(`return function ${name}(x, y) { ${body} };`)();
}

// Most quotients have a remainder. Now and then the divisor is 0, or the quotient is INT32_MIN / -1.
function dividend(i) {
    if (!(i & 1023))
        return i & 1024 ? -2147483648 : i & 31;
    return i & 31;
}

function divisor(i) {
    if (!(i & 1023))
        return i & 1024 ? -1 : 0;
    return (i & 7) + 1;
}

function test(shape) {
    let optimized = make("optimized", shape.body);
    let reference = make("reference", shape.body);
    noInline(optimized);
    noOSRExitFuzzing(optimized);
    noInline(reference);
    noDFG(reference);

    let x = shape.x || dividend;
    let y = shape.y || divisor;
    for (let i = 0; i < testLoopCount * 3; ++i) {
        let actual = optimized(x(i), y(i));
        let expected = reference(x(i), y(i));
        if (!Object.is(actual, expected))
            throw new Error(`${shape.body} with (${x(i)}, ${y(i)}) gave ${actual}, expected ${expected}`);
    }

    let compiles = numberOfDFGCompiles(optimized);
    if (compiles > shape.compiles)
        throw new Error(`${shape.body} was compiled by the DFG ${compiles} times, expected at most ${shape.compiles}`);
}

[
    // The other operand of the add is an integer, so these do not depend on how the add treats a fraction.
    { body: "return (x / 2 + 1) | 0;", compiles: 1 },
    { body: "return (1 + x / y) | 0;", compiles: 1 },
    { body: "return (x / y + (x & 7)) | 0;", compiles: 1 },
    { body: "return (x / y + 1) >>> 0;", compiles: 1 },
    { body: "return ~(x / y + 1);", compiles: 1 },
    // Rounding to the nearest integer.
    { body: "return (x / y + 0.5) | 0;", compiles: 1 },
    { body: "return (x * 100 / y + 0.5) | 0;", x: i => i & 31, y: i => (i & 7) + 1, compiles: 1 },
    // The divisor is 0 one time in eight: Infinity and NaN.
    { body: "return (x / (y - 1) + 1) | 0;", compiles: 1 },
    // Exact quotients while the function warms up, remainders after that: it exits once and that is the last time.
    { body: "return (x / y + 2) | 0;", x: i => i < testLoopCount ? (i & 31) * 4 : dividend(i), y: i => i < testLoopCount ? 4 : divisor(i), compiles: 2 },
    // This one is unchecked, and stays Int32 whatever the quotient is.
    { body: "return (x / y) | 0;", compiles: 1 },
].forEach(test);
