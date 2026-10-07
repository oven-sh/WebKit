//@ skip if not $jitTests
//@ $skipModes << :lockdown
//@ defaultNoEagerRun

// An index, a position, a radix or a byte offset that is a double made in the same function, and that is now and then
// a NaN, an Infinity or a number outside the Int32 range. FixupPhase truncates the double in place, and the truncation
// exits with Overflow on such a number. The next compile reads that exit and checks for an Int32 as before, which ends
// in an exit of kind Uncountable. The compile after that keeps the call. So there are at most three compiles.

// Each shape is two functions of different source text: the one under test, and a copy that never reaches the DFG.
function make(name, body) {
    return new Function(`
        let s = "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz";
        let a = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16];
        let view = new DataView(new ArrayBuffer(16));
        view.setUint16(0, 0x1234);
        return function ${name}(d, one) { ${body} };`)();
}

let outOfRange = [NaN, Infinity, 2147483648, -1e10];

function test(shape) {
    let optimized = make("optimized", shape.body);
    let reference = make("reference", shape.body);
    noInline(optimized);
    noOSRExitFuzzing(optimized);
    noInline(reference);
    noDFG(reference);

    // A fraction in range on three calls in four, from the first call on or after the function warmed up.
    let bad = shape.bad || outOfRange;
    for (let i = 0; i < testLoopCount * 2; ++i) {
        let d = i & 3 || (shape.late && i < testLoopCount) ? (i & 15) * 0.3 : bad[(i >> 2) % bad.length];
        let actual = optimized(d, 1);
        let expected = reference(d, 1);
        if (!Object.is(actual, expected))
            throw new Error(`${shape.body} with d = ${d} gave ${actual}, expected ${expected}`);
    }

    let compiles = numberOfDFGCompiles(optimized);
    if (compiles > 3)
        throw new Error(`${shape.body} was compiled by the DFG ${compiles} times, expected at most 3`);
}

[
    { body: "return s.at(d * one);" },
    { body: "return s.charCodeAt(d * one);", late: true },
    { body: "return s.indexOf('m', d * one);" },
    { body: "return s.slice(d * one);", late: true },
    { body: "return a.indexOf(7, d * one);" },
    { body: "return parseInt('101', d * one + 2);", late: true },
    // A NaN is byte offset 0. Every other number that is not an Int32 throws a RangeError.
    { body: "return view.getUint8(d * one);", bad: [NaN] },
    { body: "view.setUint8(d * one, 7); return view.getUint16(0);", bad: [NaN], late: true },
].forEach(test);
