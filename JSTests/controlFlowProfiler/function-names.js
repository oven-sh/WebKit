var getFunctionRanges = $vm.getFunctionRanges;

load("./driver/driver.js");

function namedFunction() { return 1; }
function anotherFunction() { return 2; }
var arrowFunc = () => 3;

// Only call namedFunction to test executed vs not-executed.
namedFunction();

var ranges = getFunctionRanges(namedFunction);

// We should have at least the functions declared in this source.
assert(ranges.length >= 3, "Expected at least 3 function ranges, got " + ranges.length);

// Find our named functions in the ranges.
var namedFunctionRange = null;
var anotherFunctionRange = null;
var arrowFuncRange = null;

for (var i = 0; i < ranges.length; i++) {
    if (ranges[i].name === "namedFunction")
        namedFunctionRange = ranges[i];
    else if (ranges[i].name === "anotherFunction")
        anotherFunctionRange = ranges[i];
    else if (ranges[i].name === "arrowFunc")
        arrowFuncRange = ranges[i];
}

// Verify named functions have their names.
assert(namedFunctionRange !== null, "Should find 'namedFunction' in ranges");
assert(anotherFunctionRange !== null, "Should find 'anotherFunction' in ranges");
assert(arrowFuncRange !== null, "Should find 'arrowFunc' in ranges");

// Verify execution status.
assert(namedFunctionRange.hasExecuted === true, "namedFunction should have executed");
assert(anotherFunctionRange.hasExecuted === false, "anotherFunction should not have executed");
assert(arrowFuncRange.hasExecuted === false, "arrowFunc should not have executed");

// Verify ranges are valid (start < end).
assert(namedFunctionRange.start < namedFunctionRange.end, "namedFunction range should be valid");
assert(anotherFunctionRange.start < anotherFunctionRange.end, "anotherFunction range should be valid");
assert(arrowFuncRange.start < arrowFuncRange.end, "arrowFunc range should be valid");
