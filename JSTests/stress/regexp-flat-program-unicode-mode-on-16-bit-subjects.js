//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")

// A pattern in unicode mode with a step that can consume a surrogate pair runs on a flat program that takes a lead surrogate and the trail surrogate behind it as one character.
// The validation runs it beside the bytecode interpreter on every match.
function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}

const patterns = [
    "^[\\p{L}\\p{N}_-]+$", "\\p{Lu}\\p{Ll}+", "[^a]+", "[^a]b$", ".", ".*", "^.*$", ".+?b", "\\P{L}", "[^\\p{L}]+", "\\B", "\\b", "(?!a)", "(?=.)", "(?:.|a)*b", "(.)\\1", "(.)(.)", "a.c", "^.$", "^..$", "^...$",
    "[\\u{1f600}-\\u{1f64f}]", "\\u{1f600}", "\\u{1f600}+", "[^\\u{1f600}]", "\\ud83d", "\\ude00", "[\\ud800-\\udbff]", "\\p{Emoji_Presentation}", "(?:\\p{L}|\\p{N})+", "^.*α.*b.*$", ".$", "[^x]b$", "\\S+", "\\s*\\S",
    "c.lef", "a.b", "ab.$", ".lef.$", "[^x]..b", "\\u2603", "[\\u2603]", ".\\u2603", "(?<=.)", "(?<=^.)x", "(?<=^[\\s\\S])x", "(?<=^..)x", "(?<!^.)x", "(?<=a.)x", "(?<=\\b.)x", "(?<![^a])x", "x(?<=.x)",
    // These can only match at the end of the subject, and the search begins there, as the bytecode interpreter's does: the two take the same turns.
    "(?=(?:.|b)*c)$", "(?!(?:.|b)*c)$", "(?=.(?:a|b){2,})$", "(?<=.)(?=(?:a|b)*c)$", "(?<=.)x$", "[\\u{1f600}-\\u{1f64f}]$", "[\\u{1f600}-\\u{1f64f}]x$", "(?=\\P{L}{0,3}?(?:\\d(\\w{1,2})ab){2,})$",
    // Where case is ignored, U+017F and U+212A are word characters, and a reference takes them for s and k.
    "\\bk\\b", "\\Bs", "\\bfoo\\b", "(k)\\1", "(s+)\\1", "(?<n>[a-z]+) \\k<n>", "(?=([a-z]*))\\1x", "(\\w)\\1+\\b", "(ß)\\1", "(\\p{L})\\1", "<(?=(\\s*))\\1(?:b|i)\\b",
];
const subjects = [
    "αβγδ_εζηθ-ικλμ42", "Αβγ δεζ", "αab", "ααb", "aαc", "α", "αα", "ααα", "😀", "a😀", "😀b", "a😀b", "😀😀", "α😀β", "αβ😀", "\ud83d", "\ude00", "a\ud83d", "\ude00b", "\ude00\ud83d", "α\ud83dβ", "\ud83d😀", "α b\nγ b", "漢字かな交じり文", "ā",
    "c\u{1c345}lef", "xa😍b", "ab😀", "x😀lefy", "y😀αb", "\u{10000}☃", "😀x", "α😀x", "a😀x", "\ude00x", "\ud83dx", "😀😀x", "xαx", "αaaa", "αaaac", "aaa😀", "1_Kab1ſab", "kK\u212a ", "\u212ak", "sSſ", "ſfoo", "foo\u212a", "ab \u212ab", "ABXα", "ßẞ", "𐐀𐐨", "<  Bα", "< iſ",
];
let matches = 0;
for (const flags of ["u", "v", "iu", "su", "mu", "gu", "yu"]) {
    for (const source of patterns) {
        let pattern;
        try {
            pattern = new RegExp(source, flags);
        } catch {
            // Under /v a class may not hold a lone "-".
            continue;
        }
        for (const subject of subjects) {
            for (const lastIndex of [0, 1, 2]) {
                pattern.lastIndex = lastIndex;
                if (pattern.exec(subject))
                    ++matches;
            }
        }
    }
}
shouldBe(matches > 1000, true, "matches");
shouldBe(new RegExp("\\B", "u").exec("a😀").index, 3, "no match begins inside a pair");
shouldBe(new RegExp("^.$", "u").test("😀"), true, "a pair is one character");
shouldBe(new RegExp("[^x]b$", "u").exec("😀b").index, 0, "a pair before the end");
shouldBe(new RegExp("c.lef", "u").exec("c\u{1c345}lef").index, 0, "a pair between characters that the search for a start looks at");
shouldBe(new RegExp("ab.$", "u").exec("ab😀").index, 0, "a pair in a match of a fixed size at the end");
shouldBe(new RegExp("(?<=^[\\s\\S])x", "u").exec("😀x").index, 2, "a pair behind");
shouldBe(new RegExp("\\bk", "iu").exec("ſk"), null, "U+017F is a word character where case is ignored");
shouldBe(new RegExp("\\bk", "u").exec("ſk").index, 1, "U+017F is no word character where case counts");
shouldBe(new RegExp("(k)\\1", "iu").exec("k\u212a").index, 0, "a reference takes U+212A for k");
shouldBe(new RegExp("(k)\\1", "i").exec("k\u212a"), null, "outside unicode mode it does not");
shouldBe(new RegExp("^[\\p{L}\\p{N}_-]+$", "u").test("αβγδ_εζηθ-ικλμ42"), true, "letters");
shouldBe(new RegExp("^[\\p{L}\\p{N}_-]+$", "u").test("αβγ😀"), false, "letters and a pair");
