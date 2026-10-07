//@ skip if not $jitTests
//@ $skipModes << :lockdown
//@ defaultNoEagerRun

// The array, radix and parseInt rows of dfg-call-intrinsic-double-index-settles.js: an index, a count or a radix that
// is a double made in the same function. FixupPhase truncates such an operand in place, as the builtin does, so the
// typed node that replaces the call stays and there is no exit at all.

// Each shape is two functions of different source text: the one under test, and a copy that never reaches the DFG.
function make(name, body) {
    return new Function(`
        let s = "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz";
        let a = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16];
        return function ${name}(x) { ${body} };`)();
}

function test(shape) {
    let optimized = make("optimized", shape.body);
    let reference = make("reference", shape.body);
    noInline(optimized);
    noOSRExitFuzzing(optimized);
    noInline(reference);
    noDFG(reference);

    for (let i = 0; i < testLoopCount; ++i) {
        let actual = optimized(i & 31);
        let expected = reference(i & 31);
        if (!Object.is(actual, expected))
            throw new Error(`${shape.body} with ${i & 31} gave ${actual}, expected ${expected}`);
    }

    let compiles = numberOfDFGCompiles(optimized);
    if (compiles > shape.compiles)
        throw new Error(`${shape.body} was compiled by the DFG ${compiles} times, expected at most ${shape.compiles}`);
}

[
    { body: "return a.slice(x / 4, 12).length;", compiles: 1 },
    { body: "return a.slice(1, x / 4 + 2.5).length;", compiles: 1 },
    { body: "return a.indexOf(7, x / 4);", compiles: 1 },
    { body: "return a.includes(7, x * 0.3);", compiles: 1 },
    { body: "let copy = [1, 2, 3, 4, 5, 6, 7, 8]; return copy.splice(x / 8, x / 16).length;", compiles: 1 },
    { body: "return (255).toString(x / 4 + 2);", compiles: 1 },
    { body: "return parseInt('101', x / 4 + 2);", compiles: 1 },
].forEach(test);
