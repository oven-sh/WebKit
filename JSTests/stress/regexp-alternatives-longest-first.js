//@ runDefault
//@ runDefault("--useRegExpImageMode=1")
//@ runDefault("--useRegExpJIT=0")
//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")
//@ runDefault("--useRegExpAlternationFactoring=0", "--useRegExpAlternationDispatch=0")

load("./resources/regexp-reference-matcher.js", "caller relative");

// More than maxAlternativesComparedPairwise (16): the analysis sweeps sorted spans instead of comparing pairs.
const manyAlternatives = "a|ab|c|d|e|f|g|h|i|j|k|l|m|n|o|p|q|r";

const orderSensitiveCases = {
    missingOrOverlappingFollower: [
        ["(?:a|ab)", "", "ab", 0, ["a"]],
        ["a|ab", "", "ab", 0, ["a"]],
        ["^(?:a|ab)", "", "ab", 0, ["a"]],
        ["x*(?:a|ab)", "", "xab", 0, ["xa"]],
        ["(?:(?:a|ab))", "", "ab", 0, ["a"]],
        ["(?:a|ab)|abc", "", "abc", 0, ["a"]],
        ["(?:a|ab)b?", "", "abb", 0, ["ab"]],
        ["(?:a|ab)(?:b|c)", "", "abc", 0, ["ab"]],
        ["(?:a|ab)[a-c]", "", "abc", 0, ["ab"]],
        ["(?:a|ab).", "", "abc", 0, ["ab"]],
        ["(?:a|ab)[^c]", "", "abd", 0, ["ab"]],
        ["(?:a|ab)(?:|b)", "", "ab", 0, ["a"]],
        ["(?:a|ab)(?:c|)", "", "abc", 0, ["a"]],
        ["(?:a|ab)c*", "", "abc", 0, ["a"]],
        ["(?:a|aB)b", "i", "abb", 0, ["ab"]],
        ["(?:a|aB)b", "i", "ABB", 0, ["AB"]],
        ["(?:a|a\\u00e9)\\u00c9", "i", "a\u00e9\u00c9", 0, ["a\u00e9"]],
        ["(?:a|a\\n)$", "m", "a\n", 0, ["a"]],
        ["(?:a{2}|a{3})a", "", "aaaa", 0, ["aaa"]],
        ["(?:a|ab)*", "", "abab", 0, ["a"]],
        ["(?:a|ab){1,2}", "", "aba", 0, ["a"]],
        ["(?:a|ab){1,2}", "", "aab", 0, ["aa"]],
        ["(?:|a)a", "", "aa", 0, ["a"]],
        ["(?:a|)a", "", "aa", 0, ["aa"]],
        ["(?:a|ab)+?b", "", "aabb", 0, ["aab"]],
        ["(?:a|ab)??b", "", "abb", 0, ["ab"]],
        ["(?:" + manyAlternatives + ")", "", "ab", 0, ["a"]],
        ["(?:" + manyAlternatives + ")b?", "", "abb", 0, ["ab"]],
        ["(?:" + manyAlternatives + ")(b?)", "", "ab", 0, ["ab", "b"]],
    ],
    capture: [
        ["(a|ab)c", "", "abc", 0, ["abc", "ab"]],
        ["(a|ab)(c|bcd)(d*)", "", "abcd", 0, ["abcd", "a", "bcd", ""]],
        ["(?:(a)|ab)c", "", "abc", 0, ["abc", null]],
        ["(?:(a)|ab)c", "", "ac", 0, ["ac", "a"]],
        ["(?:(a)|(ab))c", "", "abc", 0, ["abc", null, "ab"]],
        ["(?:a|(ab))b?", "", "abb", 0, ["ab", null]],
        ["(a|ab)\\1", "", "abab", 0, ["abab", "ab"]],
        ["(a|ab)\\1", "", "aab", 0, ["aa", "a"]],
        ["(?:a|ab)(b?)", "", "ab", 0, ["ab", "b"]],
        ["(a|ab|b)*c", "", "abc", 0, ["abc", "b"]],
        ["(a|ab)*c", "", "ababc", 0, ["ababc", "ab"]],
        ["(?=(a|ab))\\1b", "", "abb", 0, ["ab", "a"]],
        ["(?:a|ab)(?=(b?))", "", "ab", 0, ["a", "b"]],
        ["(b)?(?:a|ab)\\1", "", "ab", 0, ["a", null]],
        ["(b)?(?:a|ab)\\1", "", "babb", 0, ["bab", "b"]],
        ["(a|ab)(b?)", "", "ab", 0, ["ab", "a", "b"]],
        ["(a|ab)(c|bcd)", "", "abcd", 0, ["abcd", "a", "bcd"]],
    ],
    assertion: [
        ["(?:a|ab)\\B", "", "abc", 0, ["a"]],
        ["(?:a|a-)\\b", "", "a-", 0, ["a"]],
        ["(?:a|a-)\\b", "", "a-b", 0, ["a"]],
        ["(?:a|ab)(?=b)", "", "abb", 0, ["a"]],
        ["(?:a|ab)(?!c)", "", "abc", 0, ["a"]],
        ["(?:a\\B|ab)b", "", "abb", 0, ["ab"]],
        ["(?:a|ab)(?=b)b", "", "abb", 0, ["ab"]],
    ],
    characterBeforeGroup: [
        ["-(?:|a)\\b", "", "-a", 0, ["-"]],
        ["c(?:|a)\\B", "", "cab", 0, ["c"]],
        ["c?(?:|a)\\b", "", "a", 0, [""]],
        ["c(|a)a?\\b", "", "ca", 0, ["ca", ""]],
    ],
};

const orderInsensitivePatterns = [
    ["(?:a|ab)c", "", ["abc", "ac", "ab", "xabc"]],
    ["(?:ab|a)c", "", ["abc", "ac"]],
    ["(?:a|ab|abc)d", "", ["abcd", "abd", "ad", "abc"]],
    ["x(?:a|ab)+c", "", ["xaabc", "xabac", "xabab"]],
    ["(?:a|ab)*c", "", ["ababc", "aabac", "c"]],
    ["(?:a|ab)$", "", ["ab", "a", "aba"]],
    ["(?:a|a\\n)$", "", ["a\n", "a"]],
    ["(?:a|ab)\\b", "", ["ab", "a b", "abc"]],
    ["(?:a|ab)\\b.", "", ["ab-", "a-b"]],
    ["(?:a|ab)c*$", "", ["abc", "ab", "ac"]],
    ["(?:a{2}|a{3})b", "", ["aaab", "aab", "ab"]],
    ["(?:[ab]|[ab]c)d", "", ["acd", "bd", "bcd"]],
    ["(?:a[bc]|ab)c", "", ["abc", "acc"]],
    ["(?:a|ab)(?:c|d)", "", ["abd", "ac"]],
    ["(?:a|ab|c+)d", "", ["abd", "ad", "ccd"]],
    ["(?:a|ab){2}c", "", ["aabc", "abac", "ababc"]],
    ["^(?:a|ab)$", "", ["ab", "a"]],
    ["^(?:a|ab|abc)$", "", ["abc", "ab"]],
    ["(?:(?:a|ab)|abc)d", "", ["abcd"]],
    ["(?:a(?=b)|ab)c", "", ["abc"]],
    ["(?:a\\b|ab)c", "", ["abc"]],
    ["(?:a$|ab)", "", ["ab", "a"]],
    ["(?:a|ab)c", "u", ["abc"]],
    ["(?:\\u00e9|\\u00e9b)c", "", ["\u00e9bc", "\u00e9c"]],
    ["(?:\\u0416|\\u0416b)c", "", ["\u0416bc", "\u0416c"]],
    ["(?:" + manyAlternatives + ")z", "", ["abz", "az", "rz", "bz"]],
    ["(?:a{33}|a{34})b", "", ["a".repeat(34) + "b", "a".repeat(33) + "b"]],
    ["(?:in|instanceof)\\b", "", ["instanceof", "in", "inst"]],
    ["(?:a|ab)+c", "", ["aabc"]],
    ["c(?:|a)\\b", "", ["ca", "c", "cab", "c-"]],
    ["-(?:|-)\\b", "", ["--a", "-a", "--"]],
    ["c(|a)c?\\b", "", ["ca", "cc", "cac", "c"]],
];

const families = {
    followerAfterGroup: {
        alternations: ["a|ab", "ab|a", "a|ab|abc", "abc|a|ab", "a|b|ab", "aa|a", "a|aa|aaa", "|a", "a|", "[ab]|[ab]c"],
        groups: ["(?:X)", "(X)", "(?:X)?", "(?:X)*", "(X)*", "(?:X)+", "(?:X){2}", "(?:X){1,2}", "(?:X)*?", "(?=X)"],
        prefixes: ["", "c*"],
        followers: ["", "c", "b", "b?", "bc", "[bc]", "[^b]", "$", "\\b", "\\B", "(?=b)", "(?!b)", "(?:b|c)", "(b?)"],
        subjects: stringsOver("abc", 3).concat(["abab", "aabc", "abcc", "aaab", "ababc", "abcabc"]),
    },
    characterBeforeGroup: {
        alternations: ["|a", "a|", "|-", "|a|aa"],
        groups: ["(?:X)", "(X)", "(?:X)?", "(?:X)+", "(?:X){2}"],
        prefixes: ["c", "-", "c?", "[c-]"],
        followers: ["\\b", "\\B", "\\b-", "c?\\b", "a?\\b"],
        subjects: stringsOver("ac-", 4),
    },
};

function checkFamily({ alternations, groups, prefixes, followers, subjects }) {
    for (const alternation of alternations) {
        for (const group of groups) {
            for (const prefix of prefixes) {
                for (const follower of followers)
                    checkRegExp(prefix + group.replace("X", alternation) + follower, "", subjects);
            }
        }
    }
}

for (const cases of Object.values(orderSensitiveCases))
    checkCases(cases);

for (const [source, flags, subjects] of orderInsensitivePatterns)
    checkRegExp(source, flags, subjects);

for (const family of Object.values(families))
    checkFamily(family);
