//@ requireOptions("--compileMainScriptAheadOfTime=1")
// Ahead-of-time compiled code has no CodeBlock, which is what normally keeps a function's UnlinkedCodeBlock alive. The code still refers to
// its identifiers and constants, on paths that may not run until the collector has aged the unlinked code out.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${String(expected)} but got ${String(actual)}`);
}

function usesNamesLate(late) {
    if (!late)
        return "early";
    const object = { someProperty: "a constant string" };
    return String(new RangeError(object.someProperty + Math.max(1, 2) + 12345.678));
}
function callsIt() { try { return usesNamesLate.apply(undefined, arguments); } catch (e) { return "threw " + e; } }

shouldBe(callsIt(false), "early");
for (let i = 0; i < 20; ++i)
    fullGC();
// Something to take the place of what was freed.
const filler = [];
for (let i = 0; i < 10000; ++i)
    filler.push({ ["p" + i]: i }, "s" + i);
shouldBe(callsIt(true), "RangeError: a constant string212345.678");
