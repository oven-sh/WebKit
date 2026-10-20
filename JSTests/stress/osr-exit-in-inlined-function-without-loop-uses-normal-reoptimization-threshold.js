//@ skip if not $jitTests
//@ requireOptions("--useConcurrentJIT=false")
//@ runDefault
//@ runNoFTL

// Optimized code that exits too often is jettisoned. The exit count it tolerates is
// Options::osrExitCountForReoptimization(). For an exit in an inlined function it is the much smaller
// Options::osrExitCountForReoptimizationFromLoop() when that function once tried to enter optimized code from one of
// its loops (ScriptExecutable::didTryToEnterInLoop()). Entering optimized code at the start of a function is not such
// a try: a function with no loop must leave its callers the normal count.

if (!$vm.useDFGJIT())
    quit();

const options = jscOptions();
const normalThreshold = options.osrExitCountForReoptimization;
const loopThreshold = options.osrExitCountForReoptimizationFromLoop;
if (loopThreshold >= normalThreshold)
    throw new Error("this test needs osrExitCountForReoptimizationFromLoop < osrExitCountForReoptimization");

const warmUpCalls = 100000;

function helperWithoutLoop(exit) {
    if (exit)
        OSRExit();
    return 1;
}

function callerOfHelperWithoutLoop(exit) {
    return helperWithoutLoop(exit) + 1;
}
noInline(callerOfHelperWithoutLoop);

function helperWithLoop(exit, iterations) {
    let sum = 0;
    for (let i = 0; i < iterations; ++i)
        sum += i;
    if (exit)
        OSRExit();
    return sum;
}

function callerOfHelperWithLoop(exit) {
    return helperWithLoop(exit, 1) + 1;
}
noInline(callerOfHelperWithLoop);

// Calls the caller with an argument that makes the inlined helper exit, until the caller's optimized code is
// jettisoned. Returns the number of calls, which is the number of exits.
function exitsUntilJettison(caller) {
    if (numberOfDFGCompiles(caller) !== 1 || reoptimizationRetryCount(caller))
        throw new Error(caller.name + " is not running its first optimized code");
    const limit = 4 * normalThreshold;
    for (let calls = 1; calls <= limit; ++calls) {
        caller(true);
        if (reoptimizationRetryCount(caller))
            return calls;
    }
    throw new Error(caller.name + " was not jettisoned after " + limit + " exits");
}
noInline(exitsUntilJettison);

// The helper gets optimized code of its own, which each call enters at the start of the function.
for (let i = 0; i < warmUpCalls; ++i)
    helperWithoutLoop(false);
for (let i = 0; i < warmUpCalls; ++i)
    callerOfHelperWithoutLoop(false);

let exits = exitsUntilJettison(callerOfHelperWithoutLoop);
if (exits <= normalThreshold)
    throw new Error("an inlined function without a loop got its caller jettisoned after " + exits + " exits, expected more than " + normalThreshold);

// One long call: the loop gets hot and tries to enter optimized code.
helperWithLoop(false, warmUpCalls);
for (let i = 0; i < warmUpCalls; ++i)
    callerOfHelperWithLoop(false);

exits = exitsUntilJettison(callerOfHelperWithLoop);
if (exits > 2 * loopThreshold)
    throw new Error("an inlined function that tried to enter optimized code from its loop got its caller jettisoned after " + exits + " exits, expected about " + loopThreshold);
