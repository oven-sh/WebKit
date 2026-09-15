//@ memoryHog!
//@ slow!
//@ skip if $buildType == "debug"
//@ skip if $addressBits <= 32
//@ runDefault

// replaceAll(string, string) records the offset of every match in a Vector<size_t> before it builds
// the result. A Vector holds at most 2^31 bytes, which is 2^28 - 1 offsets. replaceAll has to throw
// when that Vector cannot grow, the way replaceAll(/regexp/g, string) does. It used to crash.
// Every block below finds more than 100 million matches, so each takes seconds.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: expected ${JSON.stringify(expected)} but got ${JSON.stringify(actual)}`);
}

function shouldThrowOutOfMemory(func) {
    let error = null;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (String(error) !== "RangeError: Out of memory")
        throw new Error(`bad error: ${String(error)}`);
}

function replaceAll(string, search, replacement) { return string.replaceAll(search, replacement); }
noInline(replaceAll);

// Warm this up so that the call below also goes through the DFG's StringReplaceAll node.
for (let i = 0; i < testLoopCount; ++i)
    shouldBe(replaceAll("banana" + i, "a", "o"), "bonono" + i);

const string = "a".repeat(2 ** 28);

shouldThrowOutOfMemory(() => string.replaceAll("a", "c"));
shouldThrowOutOfMemory(() => replaceAll(string, "a", "c"));

// An empty search string matches before every character and at the end.
shouldThrowOutOfMemory(() => string.replaceAll("", "c"));

// 2^27 matches fit.
{
    const result = string.substring(2 ** 27).replaceAll("a", "c");
    shouldBe(result.length, 2 ** 27);
    shouldBe(result[0], "c");
    shouldBe(result[2 ** 27 - 1], "c");
    shouldBe(result.indexOf("a"), -1);
}
