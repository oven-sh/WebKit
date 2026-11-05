//@ skip if $memoryLimited
//@ slow!
//@ runDefault

// The places of the pieces are gathered in a Vector, which has room for fewer of them than a string has characters.

function shouldThrowOutOfMemory(name, func) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof RangeError) || error.message !== "Out of memory")
        throw new Error(name + ": " + (error ? "threw " + error : "did not throw"));
}

function test(kind, pair) {
    const string = pair.repeat(2 ** 29);
    shouldThrowOutOfMemory(kind + " split, one character", () => string.split("a"));
    shouldThrowOutOfMemory(kind + " split, two characters", () => string.split(pair));
    shouldThrowOutOfMemory(kind + " replaceAll", () => string.replaceAll("a", "c"));
}

test("8-bit", "ab");
test("16-bit", "aĀ");

// It goes on working afterwards.
if ("a,b,c".split(",").length !== 3 || "a, b, c".split(", ").length !== 3)
    throw new Error("split after an out of memory error");
