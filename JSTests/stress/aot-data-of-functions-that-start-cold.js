//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTInlining=0", "--useAOTOperationCounters=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
function reads(o) { return o.value; }
function readsThree(o) { return o.first + o.second + o.third; }
function calls(f, x) { return f(x); }
function readsInLoop(o, n) {
    let sum = 0;
    for (let i = 0; i < n; ++i)
        sum += o.value;
    return sum;
}
function readsAfterRecursion(o, depth) {
    if (depth)
        return readsAfterRecursion(o, depth - 1) + o.value;
    return o.value;
}
function double(x) { return x * 2; }

const countsOperations = typeof aotOperationCount === "function" && aotOperationCount("operationAOTGetById") !== null && isAOTCompiled(reads) && (aotRemarks("reads") || []).includes("calls:GetById");
function operationsDuring(names, f) {
    if (!countsOperations) {
        f();
        return -1;
    }
    const count = () => names.reduce((sum, name) => sum + aotOperationCount(name), 0);
    const before = count();
    f();
    return count() - before;
}
function checkOperations(actual, atMost, what) {
    if (actual > atMost)
        throw new Error(what + ": " + actual + " calls of the operations, not at most " + atMost);
}
const readOperations = ["operationAOTGetById"];
const callOperations = ["operationAOTCacheCallee", "operationAOTCacheHostCallee"];
const warmUp = 200;

{
    const o = { unrelated: 0, value: 3 };
    checkOperations(operationsDuring(readOperations, () => {
        for (let i = 0; i < warmUp; ++i)
            check(reads(o), 3, "a read in a function that starts cold");
    }), warmUp / 4, "the first reads");
    checkOperations(operationsDuring(readOperations, () => {
        for (let i = 0; i < 2000; ++i)
            check(reads(o), 3, "a read in a function that has its own caches");
    }), 0, "later reads");
}
{
    const o = { first: 1, other: 0, second: 2, third: 4 };
    for (let i = 0; i < warmUp; ++i)
        check(readsThree(o), 7, "three reads in a function that starts cold");
    checkOperations(operationsDuring(readOperations, () => {
        for (let i = 0; i < 2000; ++i)
            check(readsThree(o), 7, "three reads in a function that has its own caches");
    }), 0, "later reads of three properties");
}
{
    for (let i = 0; i < warmUp; ++i)
        check(calls(double, i), i * 2, "a call in a function that starts cold");
    checkOperations(operationsDuring(callOperations, () => {
        for (let i = 0; i < 2000; ++i)
            check(calls(double, i), i * 2, "a call in a function that has its own caches");
    }), 0, "later calls");
}
{
    const o = { inLoop: 0, value: 2 };
    checkOperations(operationsDuring(readOperations, () => {
        check(readsInLoop(o, 5000), 10000, "reads in the loop of a function that is entered once");
    }), warmUp, "a loop in the first activation of a function");
}
{
    const o = { recursive: 0, value: 1 };
    checkOperations(operationsDuring(readOperations, () => {
        check(readsAfterRecursion(o, 400), 401, "reads in activations that began before the function had its own caches");
    }), 401, "activations that began cold");
    checkOperations(operationsDuring(readOperations, () => {
        check(readsAfterRecursion(o, 400), 401, "reads in activations that began afterwards");
    }), 0, "activations that began afterwards");
}
