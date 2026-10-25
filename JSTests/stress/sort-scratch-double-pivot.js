// The inlined sort of a short Int32 array keeps the element it is inserting in a local. Double arithmetic on the comparator's first
// parameter must not make that local a double: it is stored back into the scratch, and from there into the Int32 array.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("expected " + String(expected) + " but got " + String(actual));
}

function shouldBeArray(actual, expected) {
    shouldBe(actual.length, expected.length);
    for (let i = 0; i < expected.length; ++i) {
        if (!Object.is(actual[i], expected[i]))
            throw new Error("expected " + String(expected[i]) + " at index " + i + " but got " + String(actual[i]));
    }
}

function sortByHalves(a, b, c, d) {
    return [a, b, c, d].sort((x, y) => (x + 0.5) - (y + 0.5));
}
noInline(sortByHalves);

function sortByQuotients(a, b, c, d) {
    return [a, b, c, d].sort((x, y) => x / 2 - y / 2);
}
noInline(sortByQuotients);

function sortByFirstOnly(a, b, c, d) {
    return [a, b, c, d].sort((x, y) => (x + 0.5) - y - 0.5);
}
noInline(sortByFirstOnly);

function sortBySecondOnly(a, b, c, d) {
    return [a, b, c, d].sort((x, y) => x - (y + 0.5) + 0.5);
}
noInline(sortBySecondOnly);

function sortDescending(a, b, c, d) {
    return [a, b, c, d].sort((x, y) => (y + 0.25) - (x + 0.25));
}
noInline(sortDescending);

function sortBothWays(a, b, c, d) {
    return [a, b, c, d].sort((x, y) => ((x | 0) - (y | 0)) + (x * 0.5 - y * 0.5));
}
noInline(sortBothWays);

function sortMixed(a, b, c, d) {
    return [a, b, c, d].sort((x, y) => Number(x) - Number(y));
}
noInline(sortMixed);

function sortDoubles(a, b, c, d) {
    return [a, b, c, d].sort((x, y) => (x + 0.5) - (y + 0.5));
}
noInline(sortDoubles);

function total(array) {
    let sum = 0;
    for (let i = 0; i < array.length; ++i)
        sum += array[i];
    return sum;
}
noInline(total);

function unsortedIndexingMode(a, b, c, d) {
    return $vm.indexingMode([a, b, c, d]);
}
noInline(unsortedIndexingMode);

function checkInt32Sort(sort, a, b, c, d, expected) {
    const result = sort(a, b, c, d);
    shouldBe(result.join(), expected);
    shouldBe(total(result), a + b + c + d);
    shouldBe($vm.indexingMode(result), unsortedIndexingMode(a, b, c, d));
}

for (let i = 0; i < testLoopCount; ++i)
    shouldBe(total([i, i + 1, i + 2, i + 3]), 4 * i + 6);

const ascendingSorts = [sortByHalves, sortByQuotients, sortByFirstOnly, sortBySecondOnly, sortBothWays];

for (let i = 0; i < testLoopCount; ++i) {
    for (const sort of ascendingSorts) {
        checkInt32Sort(sort, 17, 8, 1, 1, "1,1,8,17");
        checkInt32Sort(sort, 4, 3, 2, 1, "1,2,3,4");
        checkInt32Sort(sort, -5, 2147483647, -2147483648, 0, "-2147483648,-5,0,2147483647");
    }
    checkInt32Sort(sortDescending, 17, 8, 1, 1, "17,8,1,1");
    checkInt32Sort(sortDescending, 4, 3, 2, 1, "4,3,2,1");
    checkInt32Sort(sortDescending, -5, 2147483647, -2147483648, 0, "2147483647,0,-5,-2147483648");
}

for (let i = 0; i < testLoopCount; ++i) {
    shouldBeArray(sortMixed(2.5, 1, 0.25, "3"), [0.25, 1, 2.5, "3"]);
    shouldBeArray(sortMixed(1.5, -0, "2", -1), [-1, -0, 1.5, "2"]);
    shouldBeArray(sortDoubles(2.5, 0.5, 1.5, -0.5), [-0.5, 0.5, 1.5, 2.5]);
}
