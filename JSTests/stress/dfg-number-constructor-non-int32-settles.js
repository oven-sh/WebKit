//@ skip if not $jitTests
//@ $skipModes << :lockdown
//@ defaultNoEagerRun

// Number(v) of a number that is not an Int32. A CallNumberConstructor has no arith profile, so the DFG used to turn
// every Number(number) into a DoubleAsInt32, which exits with Overflow on a fraction. No compile read that exit: each
// one made the same code again, one more DFG compile for every doubling of the call count.

// Each shape is two functions of different source text: the one under test, and a copy that never reaches the DFG.
function make(name, body) {
    return new Function(`return function ${name}(x, h) { ${body} };`)();
}

function test(shape) {
    let optimized = make("optimized", shape.body);
    let reference = make("reference", shape.body);
    noInline(optimized);
    noOSRExitFuzzing(optimized);
    noInline(reference);
    noDFG(reference);

    let x = shape.x || (i => i & 31);
    for (let i = 0; i < testLoopCount * 3; ++i) {
        let actual = optimized(x(i), 0.5);
        let expected = reference(x(i), 0.5);
        if (!Object.is(actual, expected))
            throw new Error(`${shape.body} with (${x(i)}, 0.5) gave ${actual}, expected ${expected}`);
    }

    let compiles = numberOfDFGCompiles(optimized);
    if (compiles > shape.compiles)
        throw new Error(`${shape.body} was compiled by the DFG ${compiles} times, expected at most ${shape.compiles}`);
}

[
    // A fraction from the first call on. The value profile of a call holds a few samples only, so no result of the
    // first shape is a whole number: x is 1 to 9.
    { body: "return Number(x * 0.3) | 0;", x: i => i % 9 + 1, compiles: 1 },
    { body: "return Number(h) + x;", compiles: 1 },
    { body: "return Number(h + x) * 2;", compiles: 1 },
    { body: "return Number(x) / 2;", x: i => i & 1 ? NaN : -Infinity, compiles: 1 },
    { body: "return Number(x) + h;", x: i => NaN, compiles: 1 },
    // Int32s while the function warms up, fractions after that. The argument check exits, and then the DoubleAsInt32
    // can, when the next compile started before the call returned its first fraction. That exit is the last one.
    { body: "return Number(x) + 1;", x: i => i < testLoopCount ? i & 31 : (i & 31) + 0.5, compiles: 3 },
    // These never exited and still do not. An integral double keeps its conversion to an Int32.
    { body: "return Number(x * 0.5 * 2) + 1;", compiles: 1 },
    { body: "return Number(x) + 2;", compiles: 1 },
    // An integral double outside the Int32 range: a timestamp, an id. The value profile says the same as for the
    // integral double above, so the first compile makes the DoubleAsInt32. The second one reads its Overflow exit.
    { body: "return Number(x) - 1;", x: i => 1700000000000 + (i & 31), compiles: 2 },
    { body: "return Number(x * 0.5 * 2) + 3;", x: i => i < testLoopCount ? i & 31 : (i & 31) * 4294967296, compiles: 2 },
].forEach(test);
