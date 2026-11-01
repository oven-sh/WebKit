//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")
//@ runDefault("--useRegExpImageMode=1")
//@ runDefault

// Unicode-mode patterns with many capture groups on subjects full of surrogates. The flat program skips positions where the first characters cannot match, counting a surrogate pair as one
// character, and after a failed attempt it resets only the captures that the attempt can have written. Validation compares every result with the bytecode interpreter.
function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}
const show = match => match && match.index + ":" + Array.from(match, group => group === undefined ? "-" : group).join(",");

const unused = count => "(q)".repeat(count);
const heads = ["<", "\\u{1f600}", "\\u0101", "\\ud83d", "\\ude00", "[<\\u{1f600}]", "[\\u{1f600}-\\u{1f64f}]", "[^a]", ".", "\\P{ASCII}", "(?:<|\\u{1f600})", "(<)?", "(?=(.))", "(?<=(.))", "(?<!\\u{1f600})"];
const bodies = ["(a)(b)?", "(a)|(b)", "(?:(a)|(b)|(\\u{1f601}))+", "(.)(.)(.)\\3", "(a+)(?=(b))", "(?:(a)x|(a)(b)y|(a)(b)(c))", "((a)|(\\u{1f601}))*?c", "(\\u{1f601})?(a)", "(?:(.)\\u{1f601})?(a)?<"];
const pieces = ["<", "a", "b", "c", "ab", "abc", "\u{1f600}", "\u{1f601}", "\u{e0041}", "ā", "\ud83d", "\ude00"];
const subjects = [""];
for (const first of pieces) {
    for (const second of pieces)
        subjects.push(first + second, "\u{1f600}".repeat(3) + first + second + "a", first + "\u{1f601}" + second + "abc<", "a" + first + "ab" + second + "\ud83d");
}
let matches = 0;
for (const flags of ["u", "gu"]) {
    for (const head of heads) {
        for (const body of bodies) {
            for (const source of [head + body + "|" + unused(8), head + "(?:" + body + ")" + "(q)?".repeat(8), "(q)?".repeat(8) + head + body]) {
                const pattern = new RegExp(source, flags);
                for (const subject of subjects) {
                    pattern.lastIndex = subject.length > 4 ? 1 : 0;
                    if (pattern.exec(subject))
                        ++matches;
                }
            }
        }
    }
}
shouldBe(matches > 50000, true, "matches");

const astral = "\u{e0041}".repeat(2000);
const groups = "(a)".repeat(1000);
shouldBe(new RegExp("<" + groups + "\\u{1f000}", "u").exec(astral), null, "a Latin-1 start");
shouldBe(new RegExp("\\u{1f000}" + groups, "u").exec(astral), null, "an astral start");
shouldBe(new RegExp("[\\u{e0000}-\\u{e0fff}][<>]" + groups, "u").exec(astral), null, "an astral class at the start");
shouldBe(new RegExp("\\u{e0041}<(a)" + "(b)?".repeat(1000), "u").exec(astral + "<a").index, 3998, "a match after the last pair");
shouldBe(show(new RegExp("\\u{1f601}(a)" + unused(8) + "|\\u{1f600}(b)", "u").exec("\u{1f601}a\u{1f600}b")), "3:\u{1f600}b,-,-,-,-,-,-,-,-,-,b", "captures of a failed attempt");
shouldBe(show(new RegExp("(.)(a)(b)(c)(d)(e)(f)(g)(h)|(\\u{1f600})", "u").exec("\u{1f601}abcdefg\u{1f600}")), "9:\u{1f600},-,-,-,-,-,-,-,-,-,\u{1f600}", "captures of a long failed attempt");
shouldBe(show(new RegExp("(?=(.)(a)?(b)?(c)?(d)?(e)?(f)?(g)?)\\1\\u{1f600}", "u").exec("\u{1f601}abc\u{1f601}\u{1f600}")), "5:\u{1f601}\u{1f600},\u{1f601},-,-,-,-,-,-,-", "captures kept by a lookahead");
shouldBe(show(new RegExp("\\ude00(a)?" + "(q)?".repeat(8), "u").exec("\u{1f600}a\ude00a")), "3:\ude00a,a,-,-,-,-,-,-,-,-", "a lone trail surrogate");
shouldBe(show(new RegExp("\\ud83d(a)?" + "(q)?".repeat(8), "u").exec("\u{1f600}a\ud83da")), "3:\ud83da,a,-,-,-,-,-,-,-,-", "a lone lead surrogate");
