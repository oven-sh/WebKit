//@ skip if not $jitTests
//@ $skipModes << :lockdown
//@ defaultNoEagerRun

// A call that the DFG replaces by a typed node, with an argument of a type that the node's check can never accept:
// `view.getUint16(0, x & 1)`, `s.localeCompare(5)`. The compiler proves that the check fails and ends the code there
// in an exit of kind Uncountable. Only five intrinsics read that exit back. For every other one each compile made the
// same code, which left on every call. The call now stays a call after the first such exit.

// Each shape is two functions of different source text: the one under test, and a copy that never reaches the DFG.
function make(name, body) {
    return new Function(`
        let s = "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz";
        let a = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16];
        let view = new DataView(new ArrayBuffer(16));
        view.setUint16(0, 0x1234);
        return function ${name}(x) { ${body} };`)();
}

function test(shape) {
    let optimized = make("optimized", shape.body);
    let reference = make("reference", shape.body);
    noInline(optimized);
    noOSRExitFuzzing(optimized);
    noInline(reference);
    noDFG(reference);

    for (let i = 0; i < testLoopCount * 2; ++i) {
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
    // littleEndian as an Int32, a radix that is not one, a number where a string or an object goes.
    { body: "return view.getUint16(0, x & 1);", compiles: 3 },
    { body: "return (255).toString(undefined);", compiles: 3 },
    { body: "return parseInt('10', 2.5);", compiles: 3 },
    { body: "return s.localeCompare(5);", compiles: 3 },
    { body: "return s.split(5).length;", compiles: 3 },
    { body: "return Math.max(x, undefined);", compiles: 3 },
    { body: "return Object.hasOwn(x / 2, 'a');", compiles: 3 },
    // Nothing is wrong with these arguments: the node stays, and there is no exit at all.
    { body: "return view.getUint16(0, true);", compiles: 1 },
    { body: "return Math.max(x, 3);", compiles: 1 },
].forEach(test);
