function shouldBe(actual, expected)
{
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

function shouldThrowTooLarge(source)
{
    let error;
    try {
        new RegExp(source);
    } catch (e) {
        error = e;
    }
    shouldBe(error instanceof SyntaxError, true);
    shouldBe(error.message, "Invalid regular expression: regular expression too large");
}

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
shouldBe(source.length > 1024 * 1024, true);

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

const words = [];
for (let i = 0; i < 200000; ++i)
    words.push("word" + i.toString(36));
const alternation = new RegExp(`^(?:${words.join("|")})$`);
shouldBe(alternation.source.length > 1024 * 1024, true);
shouldBe(alternation.test(words[words.length - 1]), true);
shouldBe(alternation.test("word"), false);

shouldBe(new RegExp("a|".repeat(8 * 1024 * 1024 - 1) + "ab").test("b"), false);
shouldThrowTooLarge("a|".repeat(8 * 1024 * 1024) + "a");
