// Backtracking a greedy, variable-width Unicode character class must step back one code
// point in O(1). The old YARR JIT rematched from beginIndex on every backtrack step,
// turning an inherently quadratic scan into a cubic one for patterns like /[^"]*X/u on
// 16-bit input.

function assert(b, msg) {
    if (!b)
        throw new Error("assertion failed: " + msg);
}

function assertMatch(re, input, expected) {
    var m = re.exec(input);
    var got = m ? m[0] : null;
    assert(got === expected, re + " on " + JSON.stringify(input) + " got " + JSON.stringify(got) + " expected " + JSON.stringify(expected));
}

// Correctness: backward step must give back exactly one code point (2 units for a valid
// surrogate pair, 1 for everything else including lone surrogates).
assertMatch(/([^"]*)Z/u, "abcZ", "abcZ");
assertMatch(/([^"]*)Z/u, "\u{1F600}abZ", "\u{1F600}abZ");
assertMatch(/([^"]*)Z/u, "ab\u{1F600}Z", "ab\u{1F600}Z");
assertMatch(/([^"]*)Z/u, "ab\u{1F600}cdZ", "ab\u{1F600}cdZ");
assertMatch(/([^"]*)Z/u, "\u{1F600}\u{1F600}Z", "\u{1F600}\u{1F600}Z");
assertMatch(/([^"]*)Z/u, "Z\u{1F600}\u{1F600}", "Z");
assertMatch(/([^"]*)Z/u, "aZ\u{1F600}b\u{1F600}", "aZ");
assertMatch(/([^"]*)Z/u, "\uDC00Z", "\uDC00Z");
assertMatch(/([^"]*)Z/u, "\uD800Z", "\uD800Z");
assertMatch(/([^"]*)Z/u, "\uD800\uD800Z", "\uD800\uD800Z");
assertMatch(/([^"]*)Z/u, "\uDC00\uDC00Z", "\uDC00\uDC00Z");
assertMatch(/([^"]*)Z/u, "\u{1F600}\uDC00Z", "\u{1F600}\uDC00Z");
assertMatch(/([^"]*)Z/u, "\uD800\u{1F600}Z", "\uD800\u{1F600}Z");
assertMatch(/([^"]*)Z/u, "\u{1F600}abc\u{1F601}", null);
assertMatch(/([^"]+)Z/u, "\u{1F600}Z", "\u{1F600}Z");
assertMatch(/([^"]+)Z/u, "Z", null);
assertMatch(/q([^"q]*)Z/u, "q\uDC00aZb", "q\uDC00aZ");
assertMatch(/q([^"q]*)Z/u, "q\uDC00aa", null);

// Alternating widths: "a😀" repeated, Z at the front so every step is unwound.
(function () {
    var body = "";
    for (var i = 0; i < 40; i++) body += "a\u{1F600}";
    var m = /([^"]*)Z/u.exec("Z" + body);
    assert(m[0] === "Z" && m[1] === "" && m.index === 0, "mixed-width full unwind");
})();

// Performance: 2000 BMP code units with one non-BMP leader. Prior to the O(1) backward
// step this ran ~2.3s (cubic); now it matches the interpreter at ~10ms.
(function () {
    var body = "";
    for (var i = 0; i < 2000; i++) body += "a";
    var s = "\u2014" + body;
    var start = preciseTime();
    assert(/[^"]*X/u.test(s) === false, "no match expected");
    var dt = preciseTime() - start;
    assert(dt < 1.0, "cubic backtracking regression: " + dt + "s");
})();
