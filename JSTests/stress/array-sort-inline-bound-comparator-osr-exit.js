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
const boundTwice = function (a, b) { return a - b; }.bind(null).bind(null);
const boundForLongArray = function (a, b) { return a - b; }.bind(null);

// The result of a target that exited is discarded, so what it sees after the exit has to be recorded to be checked.
const seen = [];
const boundWithThisAndArgument = function (scale, a, b, missing) {
    var difference = a - b;
    this.push(scale, a, b, missing, arguments.length);
    return difference * scale;
}.bind(seen, 1);

// The arrays are built from variables since sort isn't inlined for the copy-on-write array of a constant literal.
function useResult(x, y) { var array = [x, y]; var result = array.sort(boundForResult); return [array, result]; }
function ignoreResult(x, y) { var array = [x, y]; array.sort(boundForIgnoredResult); return [array, array]; }
function tailCall(array) { "use strict"; return array.sort(boundForTailCall); }
function callTailCall(x, y) { var array = [x, y]; return [array, tailCall(array)]; }
function withBoundTwice(x, y) { var array = [x, y]; var result = array.sort(boundTwice); return [array, result]; }
function withBoundThisAndArgument(x, y) { seen.length = 0; var array = [x, y]; var result = array.sort(boundWithThisAndArgument); return [array, result]; }
// y meets x, and overflows, once half of the array has been sorted.
function longArray(x, y) { var array = [9, 3, x, 1, 15, 4, 7, 2, y, 0, 11, 14, 13, 10, 12, 5]; var result = array.sort(boundForLongArray); return [array, result]; }

function run(sort, expected) {
    for (var i = 0; i < testLoopCount; ++i)
        sort(2, 1);

    // -5 - 2147483647 overflows int32, which is all the subtraction has seen so far.
    var [array, result] = sort(2147483647, -5);
    if (result !== array)
        throw new Error(sort.name + ": sort returned " + String(result) + " rather than its receiver");
    if (array.join() !== expected)
        throw new Error(sort.name + ": sorted to [" + array.join() + "]");
}

run(useResult, "-5,2147483647");
run(ignoreResult, "-5,2147483647");
run(callTailCall, "-5,2147483647");
run(withBoundTwice, "-5,2147483647");
run(longArray, "-5,0,1,2,3,4,5,7,9,10,11,12,13,14,15,2147483647");

run(withBoundThisAndArgument, "-5,2147483647");
if (!seen.length)
    throw new Error("withBoundThisAndArgument: the target did not run");
for (var i = 0; i < seen.length; i += 5) {
    var call = seen.slice(i, i + 5).join();
    if (call !== "1,-5,2147483647,,3" && call !== "1,2147483647,-5,,3")
        throw new Error("withBoundThisAndArgument: the target saw " + call);
}
