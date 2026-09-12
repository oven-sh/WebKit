//@ runDefault("--useRegExpJIT=true")
//@ runNoJIT("--useRegExpJIT=false")

// Non-unicode /i matching uses the committed table in YarrCanonicalizeUCS2.cpp.
// That table has to contain the BMP simple case pairs added in Unicode 16 and 17;
// /iu (generated from ucd/CaseFolding.txt at build time) already had them.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + expected + " but got " + actual);
}

function escapeForClass(ch) {
    return "\\u" + ch.charCodeAt(0).toString(16).padStart(4, "0");
}

function checkPair(lower, upper, flags) {
    const lo = String.fromCharCode(lower);
    const up = String.fromCharCode(upper);
    const name = "U+" + lower.toString(16) + "/U+" + upper.toString(16) + " /" + flags;

    for (const [pattern, input] of [[lo, up], [up, lo]]) {
        const escaped = escapeForClass(pattern);
        shouldBe(new RegExp(escaped, flags).test(input), true, name + " atom");
        shouldBe(new RegExp("^" + escaped + "$", flags).test(input), true, name + " anchored atom");
        shouldBe(new RegExp("[" + escaped + "]", flags).test(input), true, name + " class");
        shouldBe(new RegExp("[^" + escaped + "]", flags).test(input), false, name + " negated class");
        shouldBe(new RegExp("x" + escaped + "y", flags).test("x" + input + "y"), true, name + " inside a longer atom");
        shouldBe(new RegExp("(" + escaped + ")\\1", flags).test(pattern + input), true, name + " backreference");
        shouldBe(("a" + input + "b").replace(new RegExp(escaped, flags), "-"), "a-b", name + " replace");
    }
}

function checkUnrelated(a, b, flags) {
    const name = "U+" + a.toString(16) + " vs U+" + b.toString(16) + " /" + flags;
    shouldBe(new RegExp(escapeForClass(String.fromCharCode(a)), flags).test(String.fromCharCode(b)), false, name + " atom");
    shouldBe(new RegExp("[" + escapeForClass(String.fromCharCode(a)) + "]", flags).test(String.fromCharCode(b)), false, name + " class");
}

// [lower, upper]
const newPairs = [
    // Unicode 16
    [0x019b, 0xa7dc], // LATIN LETTER LAMBDA WITH STROKE
    [0x0264, 0xa7cb], // LATIN LETTER RAMS HORN
    [0xa7cd, 0xa7cc], // LATIN LETTER S WITH DIAGONAL STROKE
    [0xa7db, 0xa7da], // LATIN LETTER LAMBDA
    [0x1c8a, 0x1c89], // CYRILLIC LETTER TJE
    // Unicode 17
    [0xa7cf, 0xa7ce], // LATIN LETTER PHARYNGEAL VOICED FRICATIVE
    [0xa7d3, 0xa7d2], // LATIN LETTER DOUBLE THORN (capital is new, the small letter is from Unicode 14)
    [0xa7d5, 0xa7d4], // LATIN LETTER DOUBLE WYNN (same)
];

// Pairs next to the new entries whose table ranges were merged or split by the update.
const neighbouringPairs = [
    [0x019a, 0x023d], // LATIN LETTER L WITH BAR
    [0x0263, 0x0194], // LATIN LETTER GAMMA
    [0x0265, 0xa78d], // LATIN LETTER TURNED H
    [0xa7d1, 0xa7d0], // LATIN LETTER CLOSED INSULAR G
    [0xa7d7, 0xa7d6], // LATIN LETTER MIDDLE SCOTS S
    [0xa7d9, 0xa7d8], // LATIN LETTER SIGMOID S
    [0xa7f6, 0xa7f5], // LATIN LETTER REVERSED HALF H
    [0xa64b, 0xa64a], // CYRILLIC LETTER MONOGRAPH UK (also equivalent to U+1C88 below)
];

for (let i = 0; i < 50; ++i) {
    for (const [lower, upper] of newPairs) {
        checkPair(lower, upper, "i");
        checkPair(lower, upper, "iu");
    }
    for (const [lower, upper] of neighbouringPairs) {
        checkPair(lower, upper, "i");
        checkPair(lower, upper, "iu");
    }

    // U+1C88 CYRILLIC SMALL LETTER UNBLENDED UK is in the same set as U+A64A/U+A64B.
    checkPair(0x1c88, 0xa64a, "i");
    checkPair(0x1c88, 0xa64b, "i");

    // The new pairs must not leak into the code units around them.
    checkUnrelated(0x1c8a, 0x1c8b, "i"); // U+1C8B is unassigned
    checkUnrelated(0x1c89, 0x1c88, "i");
    checkUnrelated(0xa7cb, 0xa7ca, "i"); // U+A7CA pairs with U+A7C9
    checkUnrelated(0xa7dc, 0xa7dd, "i"); // U+A7DD is unassigned
    checkUnrelated(0xa7dc, 0xa7db, "i");
    checkUnrelated(0x019b, 0x019a, "i");
    checkUnrelated(0x0264, 0x0263, "i");
}

// Case-insensitive ranges in classes cover the new partners too. Each tested
// character lies outside the range and can only match through its partner.
shouldBe(/[\ua7cc-\ua7da]/i.test("\ua7db"), true, "U+A7DB matches through U+A7DA at the end of the range");
shouldBe(/[\u0190-\u01a0]/i.test("\ua7dc"), true, "U+A7DC matches through U+019B inside the range");
shouldBe(/[\ua7c0-\ua7ff]/i.test("\u019b"), true, "U+019B matches through U+A7DC inside the range");
shouldBe(/[\ua7c0-\ua7ff]/i.test("\u0264"), true, "U+0264 matches through U+A7CB inside the range");
shouldBe(/[\u1c80-\u1c89]/i.test("\u1c8a"), true, "U+1C8A matches through U+1C89 at the end of the range");
