function shouldBe(actual, expected)
{
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

function shouldBeTooLarge(source, flags)
{
    let error;
    try {
        new RegExp(source, flags);
    } catch (e) {
        error = e;
    }
    shouldBe(error instanceof SyntaxError, true);
    shouldBe(error.message, "Invalid regular expression: regular expression too large");
}

const limit = 1024 * 1024;
const limitWithCharacterClasses = 4 * limit;

// Few terms, long source: nearly all of it is the \u{...} escapes of the ranges.
let ranges = "";
for (let i = 0; i < 2000; ++i) {
    const from = 0xf0000 + i * 4;
    ranges += `\\u{${from.toString(16)}}-\\u{${(from + 1).toString(16)}}`;
}

const count = 40;
let source = "^";
for (let i = 1; i <= count; ++i)
    source += `(?:(?=([${ranges}]*))\\${i})x`;
source += "$";
shouldBe(source.length > limit, true);

for (const flags of ["u", "iu", "giu", "v"]) {
    const regExp = new RegExp(source, flags);
    shouldBe(regExp.source.length, source.length);
    regExp.lastIndex = 0;
    shouldBe(regExp.test("x".repeat(count)), true);
    regExp.lastIndex = 0;
    shouldBe(regExp.test("x".repeat(count - 1)), false);
    regExp.lastIndex = 0;
    shouldBe(regExp.test("\u{f0000}\u{f0005}x" + "x".repeat(count - 1)), true);
    regExp.lastIndex = 0;
    shouldBe(regExp.test("\u{f0002}x" + "x".repeat(count - 1)), false);
}

// What ends a class, and what does not.
const filler = "a".repeat(limit);
shouldBe(new RegExp(`^[${filler}\\]b]$`).test("]"), true);
shouldBe(new RegExp(`^[${filler}[]$`).test("["), true);
shouldBe(new RegExp(`^[^${filler}]$`, "v").test("b"), true);
shouldBeTooLarge(`[b]${filler}`);
shouldBeTooLarge(`[b\\]]${filler}`);
shouldBeTooLarge(`\\[${filler}]`);
shouldBeTooLarge(`[[b]${filler}`);
shouldBeTooLarge(`[[b]]${filler}`, "v");

// Only a class of characters and ranges is cheap for its length. Any other counts in full.
for (const [inClass, flags] of [["\\p{L}", "u"], ["\\P{L}", "u"], ["\\S", "u"], ["\\w", ""], ["\\d", "v"], ["[b]", "v"], ["--b", "v"], ["&&b", "v"], ["\\q{bc}", "v"]]) {
    shouldBeTooLarge(`[${filler}${inClass}]`, flags);
    shouldBeTooLarge(`[${inClass}${filler}]`, flags);
    shouldBeTooLarge(`[${filler}${inClass}`, flags);
}
shouldBeTooLarge("[\\p{L}a]".repeat(limit / 8 + 1), "u");
shouldBeTooLarge("[\\S--a]".repeat(limit / 7 + 1), "v");
shouldBeTooLarge("[[^a]]".repeat(limit / 6 + 1), "v");
shouldBeTooLarge("[a[\\p{L}]]".repeat(limit / 10 + 1), "v");
shouldBe(new RegExp(`^[${filler}]\\p{L}[\\S--a][${filler}]$`, "v").test("abba"), true);
shouldBe(new RegExp(`^[${filler}&&b]$`, "u").test("&"), true);

// Outside of classes the limit is what it was.
shouldBeTooLarge("a" + filler);
shouldBeTooLarge("a|".repeat(limit / 2) + "a");
shouldBeTooLarge("[a]".repeat(limit + 1));
shouldBeTooLarge("[" + "[a]".repeat(limit) + "]", "v");

shouldBe(new RegExp("[" + "a".repeat(limitWithCharacterClasses - 2) + "]").test("a"), true);
shouldBeTooLarge("[" + "a".repeat(limitWithCharacterClasses - 1) + "]");
