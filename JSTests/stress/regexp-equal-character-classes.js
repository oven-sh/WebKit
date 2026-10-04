//@ runDefault
//@ runDefault("--useRegExpJIT=0")

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected} but got ${actual}`);
}

const manyDistinctClasses = "(?:" + Array.from({ length: 24 }, (_, i) => `[${String.fromCharCode(0x4e00 + 3 * i)}${String.fromCharCode(0x5e00 + 5 * i)}]`).join("") + ")?";

function check(body, flags, matching, notMatching) {
    for (const prefix of ["", manyDistinctClasses]) {
        const regExp = new RegExp(`^${prefix}(?:${body})$`, flags);
        for (const string of matching)
            shouldBe(regExp.test(string), true, `/${body}/${flags} on ${JSON.stringify(string)}${prefix ? " after many classes" : ""}`);
        for (const string of notMatching)
            shouldBe(regExp.test(string), false, `/${body}/${flags} on ${JSON.stringify(string)}${prefix ? " after many classes" : ""}`);
    }
}

check("[abc]x[abc]", "", ["axb", "cxc"], ["axd", "dxa", "ax"]);
check("[abc][cba][a-c]", "", ["abc", "ccc"], ["abd", "dbc"]);
check("[abc][^abc]", "", ["ad", "c "], ["ab", "da", "a"]);
check("[^abc][abc][^abc]", "", ["xay"], ["aay", "xaa", "xxy"]);

check("(?i:[a])[a]", "", ["aa", "Aa"], ["AA", "aA"]);
check("[a](?i:[a])[a]", "", ["aaa", "aAa"], ["Aaa", "aaA"]);
check("(?-i:[k])[k]", "i", ["kk", "kK"], ["Kk", "KK"]);
check("(?-i:[k])[k]", "iu", ["kk", "kK", "kK"], ["Kk", "Kk"]);

check("kxk", "iu", ["kxk", "KXK", "KxK"], ["kxj"]);
check("sks", "iu", ["sks", "ſKſ", "SKs"], ["skz"]);
check("[k]k[K]", "iu", ["kkk", "KKk"], ["kk", "kkj"]);
check("[k]k[K]", "i", ["kkK", "KKK"], ["kkk", "Kkk"]);

check("[abc][abc]\\7", "", ["ab\x07"], ["ab7", "ab"]);
check("[abc]\\k<n>[abc]", "", ["ak<n>b"], ["ab"]);
check("(?<n>[abc])[abc]\\k<n>", "", ["aba", "ccc"], ["abb"]);

check("[\\q{ab}cd][cd]", "v", ["abc", "cd", "dd"], ["ab", "abab", "ca"]);
check("[cd][\\q{ab}cd][cd]", "v", ["cabd", "ccc"], ["abcc", "cab"]);
check("[[ab]&&[bc]][b]", "v", ["bb"], ["ab", "bc"]);
check("[[a-d]--[cd]][ab]", "v", ["ab", "ba"], ["ca", "ac"]);

check("[a\u{1f600}][a]", "u", ["aa", "\u{1f600}a"], ["a\u{1f600}", "\ud83da"]);
check("[\u{1f600}\u{1f601}][\u{1f600}\u{1f601}]", "u", ["\u{1f600}\u{1f601}", "\u{1f601}\u{1f601}"], ["\u{1f600}", "\u{1f600}\u{1f602}", "\u{1f600}\ud83d"]);
check("[\u{1f600}\u{1f601}][^\u{1f600}\u{1f601}]", "u", ["\u{1f600}a", "\u{1f601}\u{1f602}"], ["\u{1f600}\u{1f601}"]);
check("[😀][😀]", "", ["😀", "\ude00\ude00"], ["\ud83da"]);

check("[\\s\\S][^\\s\\S]|[\\s\\S][\\s\\S]", "", ["ab", "\n\n"], ["a", "abc"]);
check("[\\s\\S][^\\s\\S]|[\\s\\S][\\s\\S]", "u", ["ab", "\u{1f600}\u{1f600}"], ["a", "\u{1f600}"]);
check("[\\0-\\u{10ffff}][\\s\\S][\\0-\\u{10ffff}]", "u", ["abc", "a\u{1f600}c"], ["ab"]);
check("[\\0-\\uffff][\\0-\\u{10ffff}]", "u", ["ab", "a\u{1f600}"], ["\u{1f600}a\u{1f600}"]);

check("[abc](?<=[abc][abc])[abc]|[abc]{2}(?<=[abc][abc])[abc]", "", ["abc"], ["ab", "abd"]);
check("(?<![abc])x[abc](?![abc])", "", ["xa"], ["xab"]);
check("(?:[abc]){2,3}[abc]", "", ["abc", "abca"], ["ab", "abcab"]);
check("(?:[abc][^abc]){2}[abc]*?[^abc]", "", ["axbyz", "axbyaz"], ["axby"]);
check("([abc])+\\1[abc]", "", ["abccb"], ["abcab"]);

{
    const classes = Array.from({ length: 38 }, (_, i) => {
        let text = String.fromCharCode(0x61 + i % 26);
        for (let j = 0; j < 40; ++j)
            text += String.fromCodePoint(0x400 + 97 * i + 7 * j, 0x10400 + 131 * i + 11 * j);
        return `[${text}]`;
    });
    let source = "";
    let subject = "";
    for (let i = 0; i < 2000; ++i) {
        source += classes[i % classes.length];
        subject += i & 1 ? String.fromCharCode(0x61 + (i % classes.length) % 26) : String.fromCodePoint(0x10400 + 131 * (i % classes.length) + 11 * (i % 40));
    }
    for (const flags of ["u", "iu"]) {
        const regExp = new RegExp(`^${source}$`, flags);
        shouldBe(regExp.test(subject), true, `2000 classes, 38 distinct, /${flags}`);
        shouldBe(regExp.test(subject + "a"), false, `2000 classes, 38 distinct, /${flags}, one character too many`);
        shouldBe(regExp.test("!" + subject.slice(2)), false, `2000 classes, 38 distinct, /${flags}, a wrong first character`);
    }
}
