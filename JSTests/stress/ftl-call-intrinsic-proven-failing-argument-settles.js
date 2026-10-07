//@ skip if not $jitTests
//@ $skipModes << :lockdown
//@ requireOptions("--thresholdForFTLOptimizeAfterWarmUp=1000", "--thresholdForFTLOptimizeSoon=1000")
//@ defaultNoEagerRun

// The shapes of dfg-call-intrinsic-proven-failing-*.js behind a branch that is taken one call in four, with an FTL
// threshold so low that FTL code replaces the DFG code before that has left 100 times. The Uncountable exit that keeps
// the call a call is then one that the FTL files.

// Each shape is two functions of different source text: the one under test, and a copy that never reaches the DFG.
function make(name, body) {
    return new Function(`
        let s = "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz";
        let a = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16];
        let view = new DataView(new ArrayBuffer(16));
        view.setUint16(0, 0x1234);
        return function ${name}(x, rare) { let r = 0; if (rare) { ${body} } return r; };`)();
}

function test(shape) {
    let optimized = make("optimized", shape.body);
    let reference = make("reference", shape.body);
    noInline(optimized);
    noOSRExitFuzzing(optimized);
    noInline(reference);
    noDFG(reference);

    for (let i = 0; i < testLoopCount * 2; ++i) {
        let actual = optimized(i & 31, (i & 3) == 3);
        let expected = reference(i & 31, (i & 3) == 3);
        if (!Object.is(actual, expected))
            throw new Error(`${shape.body} with ${i & 31} gave ${actual}, expected ${expected}`);
    }

    let compiles = numberOfDFGCompiles(optimized);
    if (compiles > shape.compiles)
        throw new Error(`${shape.body} was compiled ${compiles} times, expected at most ${shape.compiles}`);
}

[
    { body: "r = s.slice(1, undefined);", compiles: 3 },
    { body: "r = s.at(undefined);", compiles: 3 },
    { body: "r = a.indexOf(7, undefined);", compiles: 3 },
    { body: "r = view.getUint16(0, x & 1);", compiles: 3 },
    // Nothing is wrong with this argument.
    { body: "r = a.indexOf(7, x >> 1);", compiles: 1 },
].forEach(test);
