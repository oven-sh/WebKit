//@ runDefault("--compileMainScriptAheadOfTime=1", "--useAOTOperationCounters=1")
//@ runDefault("--compileMainScriptAheadOfTime=1")

function shouldBe(actual, expected, what)
{
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}

const object = { property: 1 };

function asks(count)
{
    let found = 0;
    for (let i = 0; i < count; ++i) {
        if (Object.hasOwn(object, "property"))
            found++;
    }
    return found;
}

function adds(count)
{
    let sum = 0;
    for (let i = 0; i < count; ++i)
        sum = (sum + i) | 0;
    return sum;
}

function reads(o) { return o.property; }

const isCompiled = typeof isAOTCompiled === "function" && isAOTCompiled(asks);
const isCounting = isCompiled && !!jscOptions().useAOTOperationCounters;
const states = ["shared-slot", "untried-slot", "retried-slot", "filled-slot", "abandoned-slot", "abandoned-filled-slot", "polymorphic-slot", "no-slot"];

function count(name)
{
    const result = typeof aotOperationCount === "function" ? aotOperationCount(name) : null;
    shouldBe(result === null, !isCounting, "whether " + name + " is counted");
    return result;
}

function readsMany(o, count)
{
    let sum = 0;
    for (let i = 0; i < count; ++i)
        sum += reads(o);
    return sum;
}

const before = count("operationAOTHasOwnProperty");
shouldBe(asks(50), 50, "asks");
const after = count("operationAOTHasOwnProperty");
if (isCounting) {
    shouldBe(after - before, 50, "an operation is counted each time it runs");
    shouldBe(count("operationAOTHasOwn"), 0, "the beginning of a name is not a name");
    shouldBe(count("operationAOTHasOwnProperty:filled-slot"), 0, "an operation without a slot has no slot states");
    shouldBe(count("no such operation"), 0, "an unknown name");
    shouldBe(count(""), 0, "the empty name");

    shouldBe(adds(1000), 499500, "adds");
    shouldBe(count("operationAOTHasOwnProperty"), after, "an operation that does not run is not counted");

    for (let i = 0; i < 100; ++i)
        shouldBe(readsMany(object, 10), 10, "reads of one shape");
    const warm = aotOperationCount("operationAOTGetById");
    shouldBe(readsMany(object, 1000), 1000, "reads of one shape");
    shouldBe(aotOperationCount("operationAOTGetById") - warm <= 2, true, "a cached read stays in the image");
}

for (let i = 0; i < 100; ++i) {
    shouldBe(reads({ property: i }), i, "reads");
    shouldBe(reads({ other: 1, property: i }), i, "reads");
    shouldBe(reads({ ["name" + i]: 1, property: i }), i, "reads");
}
if (isCounting) {
    let byState = 0;
    for (const state of states)
        byState += count("operationAOTGetById:" + state);
    shouldBe(byState, count("operationAOTGetById"), "each arrival of an operation with a slot has one slot state");
}
