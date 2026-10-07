//@ skip if not $jitTests
//@ $skipModes << :lockdown
//@ defaultNoEagerRun

// An index, a position, a count or a radix that is a double made in the same function: `s.slice(0, s.length / 2)`,
// `s.at(x * 0.3)`. The typed node that replaces the call takes an Int32, and a double that was never a JSValue cannot
// pass a check for one: the compiled code left on every call, in every compile. FixupPhase now truncates such an
// operand in place, as the builtin does, so the node stays and there is no exit at all.

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
    { body: "return s.at(x / 2);", compiles: 1 },
    { body: "return s.charAt(x * 0.3);", compiles: 1 },
    { body: "return s.charCodeAt(x / 2);", compiles: 1 },
    { body: "return s.codePointAt(x * 0.3);", compiles: 1 },
    { body: "return s.indexOf('m', x / 2);", compiles: 1 },
    { body: "return s.lastIndexOf('m', x * 0.3);", compiles: 1 },
    { body: "return s.startsWith('ef', x / 8);", compiles: 1 },
    { body: "return s.endsWith('ef', x / 4 + 0.5);", compiles: 1 },
    { body: "return s.slice(x / 2);", compiles: 1 },
    { body: "return s.slice(0, x * 0.75);", compiles: 1 },
    { body: "return s.substring(x * 0.3, x + 0.5);", compiles: 1 },
    { body: "return s.substr(x / 2, x * 0.1);", compiles: 1 },
    // `-x` is -0 when x is 0, so the negation exits once with NegativeZero and is a double after that.
    { body: "return s.slice(-x);", compiles: 2 },
].forEach(test);
