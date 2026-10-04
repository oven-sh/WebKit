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
        if (delete object.absent)
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

const before = count("operationAOTDelById");
shouldBe(asks(50), 50, "asks");
const after = count("operationAOTDelById");
if (isCounting) {
    shouldBe(after - before, 50, "an operation is counted each time it runs");
    shouldBe(count("operationAOTDelBy"), 0, "the beginning of a name is not a name");
    shouldBe(count("operationAOTDelById:filled-slot"), 0, "an operation without a slot has no slot states");
    shouldBe(count("no such operation"), 0, "an unknown name");
    shouldBe(count(""), 0, "the empty name");

    shouldBe(adds(1000), 499500, "adds");
    shouldBe(count("operationAOTDelById"), after, "an operation that does not run is not counted");

    for (let i = 0; i < 100; ++i)
        shouldBe(readsMany(object, 10), 10, "reads of one shape");
    const warm = aotOperationCount("operationAOTGetById");
    shouldBe(readsMany(object, 1000), 1000, "reads of one shape");
    shouldBe(aotOperationCount("operationAOTGetById") - warm <= 2, true, "a cached read stays in the image");
}

function joins(array, count)
{
    let length = 0;
    for (let i = 0; i < count; ++i)
        length += array.join("-").length;
    return length;
}

shouldBe(joins([1, 2, 3], 3), 15, "joins");
const joinsBefore = count("operationArrayJoin");
shouldBe(joins([1, 2, 3], 50), 250, "joins");
if (isCounting) {
    const joined = count("operationArrayJoin") - joinsBefore;
    shouldBe(joined >= 50, true, "an operation of the other tiers is counted each time it runs (" + joined + ")");
}

function throws(value) { throw value; }

function catches(count)
{
    let caught = 0;
    for (let i = 0; i < count; ++i) {
        try {
            throws(i);
        } catch {
            caught++;
        }
    }
    return caught;
}

shouldBe(catches(3), 3, "catches");
const catchesBefore = count("operationAOTCatch");
shouldBe(catches(20), 20, "catches");
if (isCounting)
    shouldBe(count("operationAOTCatch") - catchesBefore, 20, "an operation without the usual prologue is counted each time it runs");

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

const lineOfRead = new Error().line; function readsAtOneSite(o) { return o.property; }
const lineOfStore = new Error().line; function storesAtOneSite(o, value) { o.property = value; }

function countAtLine(operation, line)
{
    let sum = 0;
    for (let column = 0; column < 120; ++column) {
        for (let offset = 0; offset < 64; ++offset)
            sum += count(operation + ":at:" + line + ":" + column + ":bc" + offset);
    }
    return sum;
}

if (isCounting) {
    const sites = [
        ["operationAOTGetById", lineOfRead, i => readsAtOneSite({ ["read" + i]: 1, property: i })],
        ["operationAOTPutById", lineOfStore, i => storesAtOneSite({ ["stored" + i]: 1 }, i)],
    ];
    for (const [operation, line, run] of sites) {
        const before = count(operation);
        const beforeAtLine = countAtLine(operation, line);
        for (let i = 0; i < 50; ++i)
            run(i);
        const arrivals = count(operation) - before;
        shouldBe(arrivals >= 50, true, operation + " arrives for each new shape");
        shouldBe(countAtLine(operation, line) - beforeAtLine, arrivals, "each arrival of " + operation + " is counted at its site");
    }
}
