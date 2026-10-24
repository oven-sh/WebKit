//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")

// A flat program whose search goes back far more often than its subject is long keeps a table of the places where the search has failed, and does not search from them again.
// A flat program that counts turns takes the turns of a search that it leaves out off the limit. The validation runs both beside the bytecode on every match, with the table from the start,
// and stops where the results or the turns differ.
function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}

const patterns = [
    "^.*a.*b.*c.*$", ".*a.*b.*", "^.*a.*b.*", ".*a.*b.*$", "^.*?a.*b.*$", "^.*(?:a|b)+c.*$", "^.*a\\s+b.*$", ".*[ab]c.*", "^.*a*.*$", "^.*\\ba.*$", "^.*(?=a)a.*b.*$", "^.*(?:a+)+b.*$",
    "^(?:.*/)?(?:.*/)?x$", "^(?:.*/)?a.*\\.b$", "^(?:[^/]*/)*[^/]*\\.b$", "^(?:(?:(?!(?:/|^)\\.).)*?/)?(?!\\.)[^/]*?\\.b$", "a.*?b.*?c", "a.*b.*?c$", "(?:a|ab)(?:c|bcd)(?:d*)$", "(a|ab)(c|bcd)(d*)$",
    "^(a+)+b$", "^(?:(a+)+b|a+c)$", "^(?:\\w+\\s*)+;$", "(?:\\w+=\\S*\\s+)*end$", "^(a*)*b$", "^(?:a*)*?b$", "^(?:a|b*)*c$", "^(?:a{1,2}){2,4}b$", "^(?:a+){2,}b$", "^(?:a?){3}a{3}$", "^(a+)\\1+b$",
    "^(?:(a)|(b)|(ab))*c$", "^(?:(a+)|(b+))*?c$", "(?:a+b?)*?c", "^(?=(a+)+b)a", "^(?!(a+)+b)a", "(?:a|a)*c|a+", "(?:a|aa)+b|c", "x(?:a.*b|a.*c)*y", "^(?:a.*?,)*b$", "^\\s*(?:\\S+\\s+)*\\S+\\s*$",
    "(?<=a)b", "(?<!a)b", "(?<=^|\\s)a", "(?<![\\w-])a+b", "(?<=\\ba)b+", "(?<=a{2})b", "(?<=ab|b)c", "(?<!$)a", "(?<=/)(?:.*/)?x$", "(?<=a)(a+)+b", "(?<=.)(?:a|a)*c", "(?<=a*)b", "(?<=(a))b",
];
const subjects = [
    "", "a", "abc", "aXbXc", "a\nb\nc", "xx a b c\nabc", "abab\nabcabc\n", "aaaaaaaaaaaa", "aaaaaaaaaaaab", "aaaaaaaaaaaac", "a/a/a/a/a/x", "a/a/a/a/a/y", "a/.a/a.b", "src/a/b/c.b", "abcd", "abbcd",
    "ab ab ab ab ;", "ab ab ab ab ab", "A=k A=k A=k end", "A=k A=k A=k no", "aabbaabbc", "aabbaabbd", "xaXbaXcy", "xaXbaXc", "a1,a2,a3,b", "a1,a2,a3,c", "  a b  c   ",
];
let matches = 0;
for (const flags of ["", "i", "m", "s", "g", "ms", "u"]) {
    for (const source of patterns) {
        const pattern = new RegExp(source, flags);
        for (const subject of subjects) {
            for (const text of [subject, subject + "ā"]) {
                for (const lastIndex of [0, 1, 3]) {
                    pattern.lastIndex = lastIndex;
                    if (pattern.exec(text))
                        ++matches;
                }
            }
        }
    }
}
shouldBe(matches > 1000, true, "matches");
