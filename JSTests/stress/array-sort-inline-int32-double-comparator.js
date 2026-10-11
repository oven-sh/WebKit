//@ defaultRun
//@ runNoCJIT("--validateGraphAtEachPhase=true")

// The inlined sort keeps the pivot of its insertion sort in a DFG variable, stores it back into the scratch
// as is, and reads the scratch of an Int32 array back with no type check. A comparator that does double
// arithmetic on its first parameter must not make that variable a double. The sort would then store boxed
// doubles into an ArrayWithInt32, compare other numbers than the elements, and return them out of order.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("bad value: " + actual + " expected: " + expected);
}

var float64 = new Float64Array([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17]);

function make(name, comparator) {
    return new Function("float64", `return function ${name}(i) { return [17, 8, i % 9 + 1, i * 7 % 9 + 1].sort(${comparator}); };`)(float64);
}

function test(optimized, expected, expectedIndexOf8) {
    for (var i = 0; i < testLoopCount; ++i) {
        var actual = optimized(i);
        // join() and indexOf() read the elements of an ArrayWithInt32 as Int32s.
        shouldBe(actual.join(), expected[i % 9]);
        shouldBe(actual.indexOf(8), expectedIndexOf8[i % 9]);
    }
}
noInline(test);

var comparators = [
    "(a, b) => (a + 0.5) - (b + 0.5)",
    "(a, b) => a - float64[b]",
];

for (var comparator of comparators) {
    var optimized = make("optimized", comparator);
    var reference = make("reference", comparator);
    noInline(optimized);
    noDFG(reference);

    var expected = [];
    var expectedIndexOf8 = [];
    for (var i = 0; i < 9; ++i) {
        var sorted = reference(i);
        expected.push(sorted.join());
        expectedIndexOf8.push(sorted.indexOf(8));
    }
    test(optimized, expected, expectedIndexOf8);
}
