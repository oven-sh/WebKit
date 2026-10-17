function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error('bad value: ' + actual + ' expected ' + expected);
}
noInline(shouldBe);

function slowDigits(n) {
    // Build the decimal digits without the int cache: one digit at a time.
    var negative = n < 0;
    var m = negative ? -n : n;
    var s = "";
    do {
        s = String.fromCharCode(48 + (m % 10)) + s;
        m = Math.floor(m / 10);
    } while (m);
    return negative ? "-" + s : s;
}
noInline(slowDigits);

function concat(n) { return "" + n; }
noInline(concat);
function prefixed(n) { return "id/" + n; }
noInline(prefixed);
function template(n) { return `${n}`; }
noInline(template);
function ctor(n) { return String(n); }
noInline(ctor);
function method(n) { return n.toString(); }
noInline(method);
function radix10(n) { return n.toString(10); }
noInline(radix10);

// A monotonic counter never hits the cache: each value is seen once.
var count = 5000000;
for (var i = 0; i < 2e4; ++i) {
    var expected = slowDigits(count);
    shouldBe(concat(count), expected);
    shouldBe(prefixed(count), "id/" + expected);
    shouldBe(template(count), expected);
    shouldBe(ctor(count), expected);
    shouldBe(method(count), expected);
    shouldBe(radix10(count), expected);
    count = (count + 1) | 0;
}

// Values seen two and three times: the second and third sightings must match the first.
for (var i = 0; i < 1e4; ++i) {
    var n = 1000000 + (i & 255);
    var expected = slowDigits(n);
    shouldBe(concat(n), expected);
    shouldBe(concat(n), expected);
    shouldBe(concat(n), expected);
}

// Edge values, each seen three times.
var edges = [1024, 1025, -1, -1024, -1025, 2147483647, -2147483648, 2147483646, -2147483647, 65536, -65536];
for (var r = 0; r < 3; ++r) {
    for (var i = 0; i < edges.length; ++i) {
        var n = edges[i];
        var expected = slowDigits(n);
        shouldBe(concat(n), expected);
        shouldBe(prefixed(n), "id/" + expected);
        shouldBe(method(n), expected);
    }
}

// Two values that alternate: each must come back correct whatever the slot holds.
for (var i = 0; i < 1e4; ++i) {
    shouldBe(concat(123456789), "123456789");
    shouldBe(concat(987654321), "987654321");
    shouldBe(concat(123456789), "123456789");
    shouldBe(concat(987654321), "987654321");
}
