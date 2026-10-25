function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("bad value: " + actual + " expected: " + expected);
}

// Double arithmetic on the comparator's first parameter must not make the inlined sort store its pivot
// back into the Int32 array as a double.
function test(a, b, c, d) {
    return [a, b, c, d].sort((x, y) => (x + 0.5) - (y + 0.5));
}
noInline(test);

for (var i = 0; i < testLoopCount; ++i)
    shouldBe(test(17, 8, 1, 1).join(), "1,1,8,17");
