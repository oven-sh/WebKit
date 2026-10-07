//@ skip if not $jitTests
//@ $skipModes << :lockdown
//@ defaultNoEagerRun

// A comparator of an inlined Array#sort that divides its parameters under a truncated add and gives a whole number:
// `(a, b) => ((a / 2 + 1) | 0) - ((b / 2 + 1) | 0)`. The checked Int32 division used to leave the compiled code on
// the first odd element, before the sort had written anything back. As a double division it does not leave, so the
// sort runs to its end in the compiled code: the array that comes back has to hold the Int32s that went in.

// Each comparator is in two functions of different source text: the one under test, and a copy that never reaches the DFG.
function make(name, comparator) {
    return new Function(`return function ${name}(v, w, x, y) { return [v, w, x, y].sort(${comparator}); };`)();
}

function test(comparator) {
    let optimized = make("optimized", comparator);
    let reference = make("reference", comparator);
    noInline(optimized);
    noInline(reference);
    noDFG(reference);

    for (let i = 0; i < Math.min(testLoopCount, 3000); ++i) {
        let x = i % 9;
        let y = i * 7 % 9;
        let actual = optimized(17, 8, x, y);
        let expected = reference(17, 8, x, y);
        for (let k = 0; k < 4; ++k) {
            if (!Object.is(actual[k], expected[k]))
                throw new Error(`sort(${comparator}) of [17, 8, ${x}, ${y}] gave [${actual}], expected [${expected}]`);
        }
    }
}

[
    "(a, b) => ((a / 2 + 1) | 0) - ((b / 2 + 1) | 0)",
    "(a, b) => ((a / 3 + 0.5) | 0) - ((b / 3 + 0.5) | 0)",
    "(a, b) => (a / 2 + 1 | 0) < (b / 2 + 1 | 0) ? -1 : 1",
].forEach(test);
