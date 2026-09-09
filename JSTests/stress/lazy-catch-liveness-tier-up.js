//@ defaultRun; run("eager-catch-liveness", "--useLazyCatchLiveness=false")

// With Options::useLazyCatchLiveness() an op_catch's value-profile buffer is created at the first DFG tier-up
// checkpoint and that tier-up is delayed once so the catch can profile; the function must still reach the DFG
// (with a catch OSR entrypoint) and compute the same thing.

function thrower(i)
{
    if (i & 1)
        throw i;
    return i;
}
noInline(thrower);

function foo(n)
{
    let sum = 0;
    for (let i = 0; i < n; ++i) {
        try {
            sum += thrower(i);
        } catch (e) {
            sum += e * 2;
        }
    }
    return sum;
}
noInline(foo);

let expected = 0;
for (let i = 0; i < 30; ++i)
    expected += (i & 1) ? i * 2 : i;

for (let i = 0; i < testLoopCount; ++i) {
    let result = foo(30);
    if (result !== expected)
        throw new Error("bad result: " + result + " at iteration " + i);
}

// numberOfDFGCompiles reads as many wherever the DFG is off; executable allocation fuzzing fails compilations at random.
if (numberOfDFGCompiles(foo) < 1 && !jscOptions().useExecutableAllocationFuzz)
    throw new Error("foo should have been compiled by the DFG");

// A catch that first executes only after foo tiered up.
function bar(n, doThrow)
{
    let sum = 0;
    for (let i = 0; i < n; ++i) {
        try {
            if (doThrow)
                throw i;
            sum += i;
        } catch (e) {
            sum += e * 3;
        }
    }
    return sum;
}
noInline(bar);

for (let i = 0; i < testLoopCount; ++i)
    bar(30, false);
for (let i = 0; i < testLoopCount; ++i) {
    let result = bar(10, true);
    if (result !== 135)
        throw new Error("bad result from bar: " + result);
}
