function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": bad value: " + actual + ", expected: " + expected);
}

function shouldBeArray(actual, expected, message) {
    shouldBe(JSON.stringify(actual), JSON.stringify(expected), message);
}

// GetSetRecord keeps the set-like's size as an unbounded integer (or +Infinity), and each method
// compares it with the receiver's size to decide whether to call other.has or iterate other.keys().
// A size of 2^32 or more must compare greater than any Set, the same as 2^32 - 1 and Infinity do.
const sizes = [2 ** 31, 2 ** 32 - 1, 2 ** 32, 2 ** 32 + 1, 2 ** 32 + 2, 2 ** 33, 2 ** 53 - 1, 2 ** 53, 2 ** 53 + 2, 2 ** 64, 1e300, Number.MAX_VALUE, Infinity];

for (const size of sizes) {
    let log = [];
    // A set-like that "contains" only 1 according to has(), and enumerates nothing from keys().
    // Which of the two the method consults is observable in the result.
    const setLike = {
        get size() { log.push("size"); return size; },
        has(v) { log.push("has:" + v); return v === 1; },
        keys() {
            log.push("keys");
            return { next() { log.push("next"); return { done: true }; } };
        },
    };

    // isSubsetOf: thisSize (1) <= otherSize, so ask other.has for each element.
    log = [];
    shouldBe(new Set([1]).isSubsetOf(setLike), true, "isSubsetOf size=" + size);
    shouldBeArray(log, ["size", "has:1"], "isSubsetOf log size=" + size);

    // isSupersetOf: thisSize (1) < otherSize, so return false without touching keys().
    log = [];
    shouldBe(new Set([1]).isSupersetOf(setLike), false, "isSupersetOf size=" + size);
    shouldBeArray(log, ["size"], "isSupersetOf log size=" + size);

    // isDisjointFrom: thisSize <= otherSize, so ask other.has for each element.
    log = [];
    shouldBe(new Set([1]).isDisjointFrom(setLike), false, "isDisjointFrom size=" + size);
    shouldBeArray(log, ["size", "has:1"], "isDisjointFrom log size=" + size);

    log = [];
    shouldBe(new Set([2, 3]).isDisjointFrom(setLike), true, "isDisjointFrom disjoint size=" + size);
    shouldBeArray(log, ["size", "has:2", "has:3"], "isDisjointFrom disjoint log size=" + size);

    // intersection: thisSize <= otherSize, so filter this by other.has.
    log = [];
    shouldBeArray([...new Set([1, 2]).intersection(setLike)], [1], "intersection size=" + size);
    shouldBeArray(log, ["size", "has:1", "has:2"], "intersection log size=" + size);

    // difference: thisSize <= otherSize, so filter this by !other.has.
    log = [];
    shouldBeArray([...new Set([1, 2]).difference(setLike)], [2], "difference size=" + size);
    shouldBeArray(log, ["size", "has:1", "has:2"], "difference log size=" + size);

    // union and symmetricDifference always iterate other.keys(); size is read but not used.
    log = [];
    shouldBeArray([...new Set([1, 2]).union(setLike)], [1, 2], "union size=" + size);
    shouldBeArray(log, ["size", "keys", "next"], "union log size=" + size);

    log = [];
    shouldBeArray([...new Set([1, 2]).symmetricDifference(setLike)], [1, 2], "symmetricDifference size=" + size);
    shouldBeArray(log, ["size", "keys", "next"], "symmetricDifference log size=" + size);
}

// Sizes below the receiver's size still take the other branch.
{
    let log = [];
    const small = {
        get size() { log.push("size"); return 1; },
        has(v) { log.push("has:" + v); return true; },
        keys() {
            log.push("keys");
            let values = [1];
            let i = 0;
            return { next() { log.push("next"); return i < values.length ? { value: values[i++], done: false } : { done: true }; } };
        },
    };

    log = [];
    shouldBe(new Set([1, 2]).isSubsetOf(small), false, "isSubsetOf small");
    shouldBeArray(log, ["size"], "isSubsetOf small log");

    log = [];
    shouldBe(new Set([1, 2]).isSupersetOf(small), true, "isSupersetOf small");
    shouldBeArray(log, ["size", "keys", "next", "next"], "isSupersetOf small log");

    log = [];
    shouldBeArray([...new Set([1, 2]).intersection(small)], [1], "intersection small");
    shouldBeArray(log, ["size", "keys", "next", "next"], "intersection small log");

    log = [];
    shouldBeArray([...new Set([1, 2]).difference(small)], [2], "difference small");
    shouldBeArray(log, ["size", "keys", "next", "next"], "difference small log");

    log = [];
    shouldBe(new Set([1, 2]).isDisjointFrom(small), false, "isDisjointFrom small");
    shouldBeArray(log, ["size", "keys", "next"], "isDisjointFrom small log");
}

// Negative sizes past -2^32 are still a RangeError, and NaN-ish sizes a TypeError.
for (const size of [-1, -(2 ** 32), -(2 ** 53), -Infinity]) {
    let threw = false;
    try {
        new Set([1]).isSubsetOf({ size, has() { return true; }, keys() { return [][Symbol.iterator](); } });
    } catch (e) {
        threw = e instanceof RangeError;
    }
    shouldBe(threw, true, "negative size=" + size);
}
for (const size of [NaN, undefined, "x"]) {
    let threw = false;
    try {
        new Set([1]).union({ size, has() { return true; }, keys() { return [][Symbol.iterator](); } });
    } catch (e) {
        threw = e instanceof TypeError;
    }
    shouldBe(threw, true, "NaN size=" + String(size));
}
