//@ skip if not $jitTests
//@ $skipModes << :lockdown
//@ requireOptions("--thresholdForFTLOptimizeAfterWarmUp=1000", "--thresholdForFTLOptimizeSoon=1000")
//@ defaultNoEagerRun

// charCodeAt and codePointAt with an index that is a double made in the same function and that is now and then past
// the end of the string. FixupPhase truncates the index in place. The node has no result for an index past the end:
// it leaves the compiled code, FTL code with an exit of kind OutOfBounds, which nothing reads. So after that exit
// the index keeps its check for Int32, which ends in an Uncountable exit, and the compile after that keeps the call.
// The FTL threshold is low, so that FTL code replaces the DFG code before that has left 100 times.

// Each shape is two functions of different source text: the one under test, and a copy that never reaches the DFG.
function make(name, body) {
    return new Function(`
        let s = "abcdefgh";
        return function ${name}(x) { ${body} };`)();
}

function test(body) {
    let optimized = make("optimized", body);
    let reference = make("reference", body);
    noInline(optimized);
    noOSRExitFuzzing(optimized);
    noInline(reference);
    noDFG(reference);

    // One index in four is past the end.
    for (let i = 0; i < testLoopCount * 2; ++i) {
        let x = (i & 3) == 3 ? 400 : (i & 7) * 2;
        let actual = optimized(x);
        let expected = reference(x);
        if (!Object.is(actual, expected))
            throw new Error(`${body} with ${x} gave ${actual}, expected ${expected}`);
    }

    let compiles = numberOfDFGCompiles(optimized);
    if (compiles > 3)
        throw new Error(`${body} was compiled ${compiles} times, expected at most 3`);
}

[
    "return s.charCodeAt(x * 0.5);",
    "return s.codePointAt(x * 0.5);",
].forEach(test);
