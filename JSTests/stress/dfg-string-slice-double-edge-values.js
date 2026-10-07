//@ skip if not $jitTests
//@ requireOptions("--thresholdForFTLOptimizeAfterWarmUp=1000", "--thresholdForFTLOptimizeSoon=1000")
//@ defaultNoEagerRun

// String.prototype.slice, substring and substr with a start, an end or a length that is a double made in the same
// function.
// FixupPhase gives such an operand to the typed node as an Int32 that it truncates in place (see
// dfg-call-intrinsic-double-index-settles.js). This checks what the node then does with every kind of number, against
// a copy of the function that never reaches the DFG: the fractions it truncates, -0, and the ones it cannot take
// (NaN, the infinities, anything outside the Int32 range), which must leave the compiled code for the same result.

function make(name, body) {
    return new Function(`return function ${name}(s, a, view, d, one) { ${body} };`)();
}

function call(func, s, a, view, d) {
    try {
        return func(s, a, view, d, 1);
    } catch (error) {
        return "threw " + error;
    }
}

// Numbers whose truncation is an Int32.
let inside = [0.5, -0.5, 1.5, -1.5, 2.5, 3, 7.9, -0, 0, 12.25, 51.99, 52, 300.7, -300.7, 2147483647, 2147483647.5, -2147483648, -2147483648.5, 1e-300, -1e-300, 0.9999999999999999];
// And the rest.
let outside = [NaN, Infinity, -Infinity, 2147483648, -2147483649, 4294967296, 4294967297.5, -4294967297.5, 9007199254740992, 1e21, -1e21, Number.MAX_VALUE, Number.MIN_VALUE];

function test(body) {
    let optimized = make("optimized", body);
    let reference = make("reference", body);
    noInline(optimized);
    noInline(reference);
    noDFG(reference);

    let s = "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz";
    let a = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16];
    let view = new DataView(new ArrayBuffer(32));
    let referenceView = new DataView(new ArrayBuffer(32));
    for (let i = 0; i < 32; ++i) {
        view.setUint8(i, i * 17 + 3);
        referenceView.setUint8(i, i * 17 + 3);
    }

    // One call in 16 gets the next special number. The others get a small fraction, so that the code is compiled
    // for a double and stays compiled while only numbers from `inside` arrive.
    let run = numbers => {
        let count = Math.max(Math.min(testLoopCount, 3000), numbers.length * 32);
        for (let i = 0; i < count; ++i) {
            let d = i & 15 ? (i & 31) * 0.3 : numbers[(i >> 4) % numbers.length];
            let actual = call(optimized, s, a, view, d);
            let expected = call(reference, s, a, referenceView, d);
            if (!Object.is(actual, expected))
                throw new Error(`${body} with d = ${Object.is(d, -0) ? "-0" : d} gave ${actual}, expected ${expected}`);
        }
    };
    run(inside);
    run(inside.concat(outside));
}

[
    "return s.slice(d * one);",
    "return s.slice(3, d * one);",
    "return s.slice(d * one, d * one + 5.5);",
    "return s.substring(d * one);",
    "return s.substring(3, d * one);",
    "return s.substring(d * one, 40);",
    "return s.substr(d * one);",
    "return s.substr(3, d * one);",
    "return s.substr(d * one, 4);",
].forEach(test);
