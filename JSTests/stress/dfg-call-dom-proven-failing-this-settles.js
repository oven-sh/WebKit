//@ skip if not $jitTests
//@ $skipModes << :lockdown
//@ defaultNoEagerRun

// A call of a function with a DOMJIT signature, with a `this` that is not a cell: `func.call(1)`. The DFG replaces the
// call by a CallDOM node that checks the class of `this`. The compiler proves that the check fails and ends the code
// there in an exit of kind Uncountable. No compile read that exit, so each one made the same code again. The call now
// stays a call after the first such exit.

let dom = $vm.createDOMJITFunctionObject();

function make(body) {
    return new Function("func", "dom", `return function optimized(x) { "use strict"; ${body} };`)(dom.func, dom);
}

// The host function throws a TypeError for a `this` of another class. The catch is out here, so that the function under
// test has no exit of its own for the exception.
function call(func, x) {
    try {
        return func(x);
    } catch (error) {
        return String(error);
    }
}

function test(shape) {
    let optimized = make(shape.body);
    noInline(optimized);
    noOSRExitFuzzing(optimized);

    // Every call of the first shape throws, so it gets no more calls than it takes to tell two compiles from many.
    for (let i = 0; i < shape.calls; ++i) {
        let actual = call(optimized, i & 31);
        if (actual !== shape.expected)
            throw new Error(`${shape.body} gave ${actual}, expected ${shape.expected}`);
    }

    let compiles = numberOfDFGCompiles(optimized);
    if (compiles > shape.compiles)
        throw new Error(`${shape.body} was compiled by the DFG ${compiles} times, expected at most ${shape.compiles}`);
}

[
    { body: "return func.call(1);", expected: "TypeError: Type error", calls: testLoopCount / 2, compiles: 3 },
    // The right `this`: the CallDOM node stays, and there is no exit at all.
    { body: "return dom.func();", expected: 42, calls: testLoopCount, compiles: 1 },
].forEach(test);
