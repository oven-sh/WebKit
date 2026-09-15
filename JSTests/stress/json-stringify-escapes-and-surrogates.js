// Exercises the segmented (vectorized) JSON string escaping: escapes and surrogates
// at every position relative to the vector stride, runs of characters needing
// escaping, surrogate pairs straddling stride boundaries, and outputs large enough
// to leave the fast stringifier's static buffer.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("bad value: " + actual + ", expected: " + expected);
}

// Known answers.
shouldBe(JSON.stringify('"'), '"\\""');
shouldBe(JSON.stringify("\\"), '"\\\\"');
shouldBe(JSON.stringify("\n"), '"\\n"');
shouldBe(JSON.stringify("\t"), '"\\t"');
shouldBe(JSON.stringify("\r"), '"\\r"');
shouldBe(JSON.stringify("\b"), '"\\b"');
shouldBe(JSON.stringify("\f"), '"\\f"');
shouldBe(JSON.stringify("\x00"), '"\\u0000"');
shouldBe(JSON.stringify("\x1f"), '"\\u001f"');
shouldBe(JSON.stringify("\x7f"), '"\x7f"'); // DEL is not escaped.
shouldBe(JSON.stringify("\uD83C\uDF0D"), '"\uD83C\uDF0D"'); // Valid pair stays literal.
shouldBe(JSON.stringify("\uD800"), '"\\ud800"'); // Lone lead.
shouldBe(JSON.stringify("\uDC00"), '"\\udc00"'); // Lone trail.
shouldBe(JSON.stringify("a\uD800b"), '"a\\ud800b"');
shouldBe(JSON.stringify("\uD800\uD800"), '"\\ud800\\ud800"');
shouldBe(JSON.stringify("\uDC00\uD83C\uDF0D\uD800"), '"\\udc00\uD83C\uDF0D\\ud800"');
shouldBe(JSON.stringify("\uDBFF\uDFFF"), '"\uDBFF\uDFFF"'); // Highest valid pair.
shouldBe(JSON.stringify("\uD7FF\uE000"), '"\uD7FF\uE000"'); // Neighbors of the surrogate range.

// Every special character at every position relative to the 16 (8-bit) and
// 8 (16-bit) character strides, with round-trip verification.
const specials = ['"', "\\", "\n", "\x00", "\x1f", "\uD800", "\uDC00", "\uD83C\uDF0D"];
const base8 = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLM";
const base16 = "\u6C49\u5B57\u6C34\u706B\u571F\u91D1\u6728\u65E5\u6708\u5C71\u5DDD\u7530\u4E2D\u732B\u72AC\u9CE5\u9B5A\u866B\u8349\u82B1\u96E8\u96EA\u98A8\u6F22\u5B50\u6C350\u706B0\u571F0\u91D10\u6C49\u5B57\u6C34\u706B\u571F\u91D1\u6728\u65E5\u6708\u5C71".slice(0, 40);
for (const base of [base8, base16]) {
    for (let length = 0; length <= 40; ++length) {
        for (let position = 0; position <= length; ++position) {
            for (const special of specials) {
                const string = base.slice(0, position) + special + base.slice(position, length);
                shouldBe(JSON.parse(JSON.stringify(string)), string);
            }
        }
    }
}

// Runs of characters needing special handling.
shouldBe(JSON.stringify("\n".repeat(64)), '"' + "\\n".repeat(64) + '"');
shouldBe(JSON.stringify('"'.repeat(33)), '"' + '\\"'.repeat(33) + '"');
shouldBe(JSON.stringify("\uD800".repeat(20)), '"' + "\\ud800".repeat(20) + '"');
shouldBe(JSON.stringify("\uD83C\uDF0D".repeat(20)), '"' + "\uD83C\uDF0D".repeat(20) + '"');
shouldBe(JSON.parse(JSON.stringify("\x00\x01\x02\x03\x04\x05\x06\x07\b\t\n\x0b\f\r\x0e\x0f".repeat(8))), "\x00\x01\x02\x03\x04\x05\x06\x07\b\t\n\x0b\f\r\x0e\x0f".repeat(8));

// Surrogate pairs straddling stride boundaries, lone surrogates at string edges.
for (let pad = 0; pad <= 20; ++pad) {
    const padding = "\u6C49".repeat(pad);
    let string = padding + "\uD83C\uDF0D" + padding;
    shouldBe(JSON.parse(JSON.stringify(string)), string);
    string = padding + "\uD800";
    shouldBe(JSON.parse(JSON.stringify(string)), string);
    string = "\uDC00" + padding;
    shouldBe(JSON.parse(JSON.stringify(string)), string);
}

// Outputs past the fast stringifier's static buffer, in both string widths.
const big8 = 'lorem ipsum "quoted" and\\slashed\n'.repeat(600);
shouldBe(JSON.parse(JSON.stringify(big8)), big8);
const big16 = '\u6F22\u5B57\uD83C\uDF0D\u30C6\u30B9\u30C8\n"\u5F15\u7528"'.repeat(800);
shouldBe(JSON.parse(JSON.stringify(big16)), big16);
const lateEscape = "x".repeat(9000) + "\n";
shouldBe(JSON.parse(JSON.stringify(lateEscape)), lateEscape);

// The fast stringifier and the general stringifier must agree (an empty string gap
// forces the general path).
const values = [
    big8,
    big16,
    { 'a"b': 'he said "hello"\nthen left\t\\some\\path', emoji: "\uD83C\uDF0D plus \uD800 lone", arr: ["\x01", "\u65E5\u672C"] },
    ["clean", '"', "\uD800", "\uD83C\uDF0D", "\u6F22", "\n".repeat(40)],
];
for (const value of values)
    shouldBe(JSON.stringify(value), JSON.stringify(value, null, ""));
