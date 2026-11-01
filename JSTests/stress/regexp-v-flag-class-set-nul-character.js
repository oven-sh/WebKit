function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: got ${actual}, expected ${expected}`);
}

function shouldThrowSyntaxError(source) {
    let error = null;
    try {
        new RegExp(source, "v");
    } catch (e) {
        error = e;
    }
    if (!(error instanceof SyntaxError))
        throw new Error(`Expected SyntaxError for ${JSON.stringify(source)} with v but got ${error}`);
}

// A raw U+0000 is a SourceCharacter that is neither a ClassSetSyntaxCharacter nor half of a
// ClassSetReservedDoublePunctuator, so it is a valid ClassSetCharacter in a /v class set and in a
// \q{} class string, the same as it is in a /u character class and outside a class.
const NUL = "\0";

for (const [source, matching, nonMatching] of [
    ["^[" + NUL + "]$", [NUL], ["a", "0", ""]],
    ["^[a" + NUL + "]$", [NUL, "a"], ["b"]],
    ["^[" + NUL + "a]$", [NUL, "a"], ["b"]],
    ["^[" + NUL + NUL + "]$", [NUL], ["a"]],
    ["^[" + NUL + "-a]$", [NUL, "\x01", "A", "a"], ["b"]],
    ["^[\\x00-" + NUL + "]$", [NUL], ["\x01"]],
    ["^[^" + NUL + "]$", ["a", "\x01"], [NUL]],
    ["^[[" + NUL + "]]$", [NUL], ["a"]],
    ["^[[a-z]" + NUL + "]$", [NUL, "q"], ["Q"]],
    ["^[\\q{" + NUL + "}]$", [NUL], ["a", ""]],
    ["^[\\q{a" + NUL + "b|c}]$", ["a" + NUL + "b", "c"], ["ab", NUL]],
    ["^[" + NUL + "&&" + NUL + "]$", [NUL], ["a"]],
    ["^[\\x00&&" + NUL + "]$", [NUL], ["a"]],
    ["^[a&&" + NUL + "]$", [], [NUL, "a"]],
    ["^[\\w--" + NUL + "]$", ["a"], [NUL]],
    ["^[[" + NUL + "a]--" + NUL + "]$", ["a"], [NUL]],
]) {
    const regExp = new RegExp(source, "v");
    for (const string of matching)
        shouldBe(regExp.test(string), true, `${JSON.stringify(source)} with v should match ${JSON.stringify(string)}`);
    for (const string of nonMatching)
        shouldBe(regExp.test(string), false, `${JSON.stringify(source)} with v should not match ${JSON.stringify(string)}`);
}

// The composition documented for RegExp.escape, which leaves U+0000 as is.
{
    const input = "a" + NUL + "b";
    const regExp = new RegExp("[" + RegExp.escape(input) + "]", "v");
    shouldBe(regExp.test(NUL), true, "escaped class should match NUL");
    shouldBe(regExp.test("a"), true, "escaped class should match a");
    shouldBe(regExp.test("c"), false, "escaped class should not match c");
}

// Case-insensitive and with other flags.
shouldBe(new RegExp("[" + NUL + "]", "vi").test(NUL), true, "vi");
shouldBe(new RegExp("[" + NUL + "]", "vg").test("x" + NUL), true, "vg");

// The ClassSetSyntaxCharacters and reserved double punctuators next to a NUL are still rejected.
shouldThrowSyntaxError("[" + NUL + "(]");
shouldThrowSyntaxError("[" + NUL + "|]");
shouldThrowSyntaxError("[" + NUL + "&&&a]");
shouldThrowSyntaxError("[" + NUL + "!!" + "]");
shouldThrowSyntaxError("[" + NUL + "-]");
shouldThrowSyntaxError("[\\q{" + NUL + "(}]");
// An escaped backslash before NUL is still not an identity escape in Unicode mode.
shouldThrowSyntaxError("[\\" + NUL + "]");
