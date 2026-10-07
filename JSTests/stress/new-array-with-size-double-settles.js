//@ skip if not $jitTests
//@ $skipModes << :lockdown
//@ defaultNoEagerRun

// `new Array(n)` and `Array(n)` with a length that is a double made in the same function: `new Array(2 ** k)`,
// `new Array(half * 2)`. NewArrayWithSize takes the length as an Int32, and a double that was never a JSValue cannot
// pass a check for one: the compiled code left on every call, in every compile, also for a whole number. FixupPhase
// now converts such a length in place. The conversion is exact: a fraction is a RangeError and still leaves.

// Each shape is two functions of different source text: the one under test, and a copy that never reaches the DFG.
function make(name, body) {
    return new Function(`return function ${name}(x) { ${body} };`)();
}

function call(func, x) {
    try {
        return func(x);
    } catch (error) {
        return "threw " + error;
    }
}

function test(shape) {
    let optimized = make("optimized", shape.body);
    let reference = make("reference", shape.body);
    noInline(optimized);
    noOSRExitFuzzing(optimized);
    noInline(reference);
    noDFG(reference);

    let x = shape.x || (i => i & 7);
    for (let i = 0; i < testLoopCount; ++i) {
        let actual = call(optimized, x(i));
        let expected = call(reference, x(i));
        if (!Object.is(actual, expected))
            throw new Error(`${shape.body} with ${x(i)} gave ${actual}, expected ${expected}`);
    }

    let compiles = numberOfDFGCompiles(optimized);
    if (compiles > shape.compiles)
        throw new Error(`${shape.body} was compiled by the DFG ${compiles} times, expected at most ${shape.compiles}`);
}

// Lengths that are not a small whole number: a RangeError, 0 for -0, or an array that is not allocated inline.
let lengths = [2.5, -0, -1, NaN, 0.5, Infinity, 4294967295, 4294967296, -0.5, 2147483648, 1e21, 3];

[
    { body: "return new Array(x * 0.5 * 2).length;", compiles: 1 },
    { body: "return Array(x * 0.5 * 2).length;", compiles: 1 },
    { body: "return new Array(2 ** (x & 3)).length;", compiles: 1 },
    { body: "let half = x / 2; return new Array(half + half).length;", compiles: 1 },
    // Only the values are checked here: a length that the conversion does not take leaves the compiled code.
    { body: "return new Array(x * 0.5 * 2).length + 1;", x: i => i & 31 ? i & 7 : lengths[(i >> 5) % lengths.length], compiles: Infinity },
].forEach(test);
