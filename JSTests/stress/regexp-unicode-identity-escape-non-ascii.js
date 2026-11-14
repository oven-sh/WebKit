function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`FAIL: expected '${expected}' actual '${actual}'`);
}

function shouldThrowSyntaxError(func) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof SyntaxError))
        throw new Error(`FAIL: expected SyntaxError, got ${error}`);
}

// In UnicodeMode an IdentityEscape is a SyntaxCharacter or '/'. Any other
// escaped character, ASCII or not, is a SyntaxError.
for (const ch of ["\u00e9", "\u00c7", "\u4e2d", "\u5b57", "\u{1F600}", "\u{1D4B3}", "\ud83d", "\ude00", "\u2028", "\ufeff"]) {
    for (const flags of ["u", "v"]) {
        shouldThrowSyntaxError(() => new RegExp("\\" + ch, flags));
        shouldThrowSyntaxError(() => new RegExp("[\\" + ch + "]", flags));
        shouldThrowSyntaxError(() => new RegExp("(?:\\" + ch + ")+", flags));
    }
    shouldThrowSyntaxError(() => new RegExp("[\\q{\\" + ch + "}]", "v"));
    shouldThrowSyntaxError(() => new RegExp("[[a]--[\\" + ch + "]]", "v"));
}

// The allowed UnicodeMode identity escapes still work.
for (const ch of "^$\\.*+?()[]{}|/") {
    for (const flags of ["u", "v"])
        shouldBe(new RegExp("^\\" + ch + "$", flags).test(ch), true);
}
shouldBe(/^[\-]$/u.test("-"), true);
shouldBe(/^[\]]$/u.test("]"), true);
shouldBe(/^[\&]$/v.test("&"), true);

// NUL and an ASCII character outside the allowed set were errors before. They still are.
for (const flags of ["u", "v"]) {
    shouldThrowSyntaxError(() => new RegExp("\\\u0000", flags));
    shouldThrowSyntaxError(() => new RegExp("\\a", flags));
    shouldThrowSyntaxError(() => new RegExp("\\q", flags));
}

// A non-ASCII character that is not escaped, or that is written as a unicode escape, still matches.
for (const flags of ["u", "v"]) {
    shouldBe(new RegExp("^\u00c7$", flags).test("\u00c7"), true);
    shouldBe(new RegExp("^\\u00C7$", flags).test("\u00c7"), true);
    shouldBe(new RegExp("^\\u{1D4B3}$", flags).test("\u{1D4B3}"), true);
    shouldBe(new RegExp("^[\u{1D4B3}]$", flags).test("\u{1D4B3}"), true);
}

// Without the unicode flags the escape stays an identity escape (Annex B).
for (const ch of ["\u00e9", "\u00c7", "\u4e2d", "\u{1F600}"]) {
    shouldBe(new RegExp("^\\" + ch + "$").test(ch), true);
    shouldBe(new RegExp("^[\\" + ch + "]+$").test(ch), true);
}
