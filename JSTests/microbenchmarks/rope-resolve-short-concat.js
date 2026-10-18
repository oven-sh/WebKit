// Each id is built with + or += and then read, which resolves the rope. A resolve notifies its
// MarkedBlock that a cell now needs destruction. The three loops differ in how often that
// notification is the first one in a block: almost never, once per block, and almost always.

function prefixAndCounter(prefix, count, iterations) {
    var checksum = 0;
    for (var i = 0; i < iterations; ++i) {
        var id = prefix + count;
        count = (count + 1) | 0;
        checksum = (checksum + id.length + id.charCodeAt(0)) >>> 0;
    }
    return checksum;
}
noInline(prefixAndCounter);

var alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_";

function appendCharacters(appends, iterations) {
    var checksum = 0;
    for (var i = 0; i < iterations; ++i) {
        var id = "";
        for (var j = 0; j < appends; ++j)
            id += alphabet[(i + j) & 63];
        checksum = (checksum + id.length + id.charCodeAt(0)) >>> 0;
    }
    return checksum;
}
noInline(appendCharacters);

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("bad value: " + actual + " expected " + expected);
}

shouldBe(prefixAndCounter("0123456789abcdef012345/", 5000000, 1e6), 78000000);
shouldBe(appendCharacters(21, 1e5), 10735360);
shouldBe(appendCharacters(600, 4000), 2744860);
