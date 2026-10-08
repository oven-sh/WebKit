//@ skip if not $jitTests
//@ $skipModes << :lockdown
//@ defaultNoEagerRun

// `++v` and `--v` on a local at the end of the Int32 range, with a result that is only truncated. FixupPhase used to
// give an Int32 Inc or Dec Arith::CheckOverflow whatever uses the result, so it exits with Overflow there. The DFG
// reads "the result is not used as a number" as "may stay Int32 although it overflows", which is right for a node
// that is unchecked then, and made the same checked code in every compile. An Inc or Dec whose result is only
// truncated is now unchecked, like the add or the subtraction it becomes.

// Each shape is two functions of different source text: the one under test, and a copy that never reaches the DFG.
function make(name, body) {
    return new Function(`return function ${name}(x) { ${body} };`)();
}

function test(shape) {
    let optimized = make("optimized", shape.body);
    let reference = make("reference", shape.body);
    noInline(optimized);
    noOSRExitFuzzing(optimized);
    noInline(reference);
    noDFG(reference);

    // One call in four is at the boundary.
    for (let i = 0; i < testLoopCount * 3; ++i) {
        let x = i & 3 ? i & 31 : shape.boundary;
        let actual = optimized(x);
        let expected = reference(x);
        if (!Object.is(actual, expected))
            throw new Error(`${shape.body} with ${x} gave ${actual}, expected ${expected}`);
    }

    let compiles = numberOfDFGCompiles(optimized);
    if (compiles > shape.compiles)
        throw new Error(`${shape.body} was compiled by the DFG ${compiles} times, expected at most ${shape.compiles}`);
}

[
    // The result is only truncated: the Inc or Dec wraps, and there is no exit.
    { body: "let v = x; return ++v | 0;", boundary: 2147483647, compiles: 1 },
    { body: "let v = x; return --v | 0;", boundary: -2147483648, compiles: 1 },
    { body: "let v = x; v++; return v >>> 0;", boundary: 2147483647, compiles: 1 },
    { body: "let v = x; v--; return ~v;", boundary: -2147483648, compiles: 1 },
    { body: "let v = x; return (++v + 1) | 0;", boundary: 2147483647, compiles: 1 },
    { body: "let v = x; v++; v++; return v & 0xffff;", boundary: 2147483646, compiles: 1 },
    // The result is used as a number: the Inc or Dec is checked, and a double once it overflowed. A subtraction
    // counts as such a use whatever its other operand is.
    { body: "let v = x; return (++v + 0.5) | 0;", boundary: 2147483647, compiles: 2 },
    { body: "let v = x; return (--v + 0.5) | 0;", boundary: -2147483648, compiles: 2 },
    { body: "let v = x; return (--v - 1) | 0;", boundary: -2147483648, compiles: 2 },
    { body: "let v = x; return (++v * 3) | 0;", boundary: 2147483647, compiles: 2 },
    { body: "let v = x; return ++v;", boundary: 2147483647, compiles: 2 },
    { body: "let v = x; v--; return v / 2;", boundary: -2147483648, compiles: 2 },
].forEach(test);
