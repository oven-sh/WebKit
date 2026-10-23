//@ defaultRun; run("osr-exit-to-llint", "--forceOSRExitToLLInt=1", "--useConcurrentJIT=0")

// When the comparator given to an inlined Array.prototype.sort is a bound function, the DFG inlines the bound
// function's target in its place. An OSR exit inside the target must still return through
// arraySortComparatorReturnTrampoline, which runs the call to sort again. Returning to the return location of the
// call to sort instead makes the comparator's result the result of sort and leaves the array unsorted, and a tail
// call to sort has no return location at all.

// Each case gets a target of its own: once one has overflowed, its profile stops later compilations from speculating.
const boundForResult = function (a, b) { return a - b; }.bind(null);
const boundForIgnoredResult = function (a, b) { return a - b; }.bind(null);
const boundForTailCall = function (a, b) { return a - b; }.bind(null);
const boundWithArgument = function (scale, a, b) { return (a - b) * scale; }.bind(null, 1);
const boundTwice = function (a, b) { return a - b; }.bind(null).bind(null);

// The arrays are built from variables since sort isn't inlined for the copy-on-write array of a constant literal.
function useResult(x, y) { var array = [x, y]; var result = array.sort(boundForResult); return [array, result]; }
function ignoreResult(x, y) { var array = [x, y]; array.sort(boundForIgnoredResult); return [array, array]; }
function tailCall(array) { "use strict"; return array.sort(boundForTailCall); }
function callTailCall(x, y) { var array = [x, y]; return [array, tailCall(array)]; }
function withBoundArgument(x, y) { var array = [x, y]; var result = array.sort(boundWithArgument); return [array, result]; }
function withBoundTwice(x, y) { var array = [x, y]; var result = array.sort(boundTwice); return [array, result]; }

function run(sort) {
    for (var i = 0; i < testLoopCount; ++i)
        sort(2, 1);

    // -5 - 2147483647 overflows int32, which is all the subtraction has seen so far.
    var [array, result] = sort(2147483647, -5);
    if (result !== array)
        throw new Error(sort.name + ": sort returned " + String(result) + " rather than its receiver");
    if (array.length !== 2 || array[0] !== -5 || array[1] !== 2147483647)
        throw new Error(sort.name + ": sorted to [" + array.join() + "]");
}

run(useResult);
run(ignoreResult);
run(callTailCall);
run(withBoundArgument);
run(withBoundTwice);
