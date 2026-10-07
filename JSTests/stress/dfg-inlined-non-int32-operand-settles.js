//@ skip if not $jitTests
//@ $skipModes << :lockdown
//@ defaultNoEagerRun

// The shapes of the other *-settles.js tests, in a helper that the DFG inlines into the function whose compiles are
// counted. The exit is then filed on the block of the helper, while the code that is thrown away is the caller's. The
// next compile of the caller has to find that exit through the inlined frame.

// Each shape is two functions of different source text: the one under test, and a copy that never reaches the DFG.
function make(name, body) {
    return new Function(`
        let s = "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz";
        function ${name}Helper(x, y) { ${body} }
        function ${name}(x, y) { return ${name}Helper(x, y); }
        return [${name}, ${name}Helper];`)();
}

function test(shape) {
    let [optimized] = make("optimized", shape.body);
    let [reference, referenceHelper] = make("reference", shape.body);
    noInline(optimized);
    noOSRExitFuzzing(optimized);
    noInline(reference);
    noDFG(reference);
    noDFG(referenceHelper);

    let x = shape.x || (i => i & 31);
    let y = shape.y || (i => (i & 7) + 1);
    for (let i = 0; i < testLoopCount * 2; ++i) {
        let actual = optimized(x(i), y(i));
        let expected = reference(x(i), y(i));
        if (!Object.is(actual, expected))
            throw new Error(`${shape.body} with (${x(i)}, ${y(i)}) gave ${actual}, expected ${expected}`);
    }

    let compiles = numberOfDFGCompiles(optimized);
    if (compiles > shape.compiles)
        throw new Error(`inlined ${shape.body}: the caller was compiled by the DFG ${compiles} times, expected at most ${shape.compiles}`);
}

[
    // FixupPhase truncates the double in the inlined frame, and the division under the add is a double division. The
    // first compile can come before the helper has a profile of its division: then that division is an Int32 division
    // once, and exits with Overflow.
    { body: "return s.slice(x / 2);", compiles: 2 },
    { body: "return (x / y + 1) | 0;", compiles: 2 },
    // The Uncountable exit is filed on the helper, and the call in the helper stays a call.
    { body: "return s.slice(1, undefined);", compiles: 3 },
    // An Overflow exit in the inlined frame: Number() of a fraction, and a division, after an Int32 warm-up.
    { body: "return Number(x) + 1;", x: i => i < testLoopCount ? i & 31 : (i & 31) + 0.5, compiles: 3 },
    { body: "return (x / y + 2) | 0;", x: i => i < testLoopCount ? (i & 31) * 4 : i & 31, y: i => i < testLoopCount ? 4 : (i & 7) + 1, compiles: 3 },
].forEach(test);
