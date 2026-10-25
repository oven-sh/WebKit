//@ runDefault
//@ runDefault("--useRegExpImageMode=1")
//@ runDefault("--useRegExpJIT=0")
//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")

load("./resources/regexp-reference-matcher.js", "caller relative");

const maxGroupsCrossedToRepeatedLookahead = 8;
const nested = (text, depth) => "(?:".repeat(depth) + text + ")".repeat(depth);

const skipsComparison = {
    adjacent: [
        ["(?=(a+))\\1", "", "aaab", 0, ["aaa", "aaa"]],
        ["(?=(a+))\\1b", "", "aaab", 0, ["aaab", "aaa"]],
        ["(?=(a+))\\1b", "", "aaa", null, null],
        ["(?=(a+))\\1b", "", "baab", 1, ["aab", "aa"]],
        ["(?=(a+))\\1a", "", "aaa", null, null],
        ["(?=(a*))\\1b", "", "b", 0, ["b", ""]],
        ["(?=(a+))\\1bc", "", "aabc", 0, ["aabc", "aa"]],
        ["(?=(a+))\\1bc", "", "aab", null, null],
        ["(?=(?<x>a+))\\k<x>b", "", "aab", 0, ["aab", "aa"]],
        ["(?=(a+))\\1", "i", "aAb", 0, ["aA", "aA"]],
        ["x(?=(a+))\\1$", "", "xaa", 0, ["xaa", "aa"]],
        ["x(?=(a+))\\1$", "", "xaab", null, null],
    ],
    mandatoryQuantifiedLookahead: [
        ["(?=(a)){2}\\1", "", "aa", 0, ["a", "a"]],
    ],
    assertionsBetween: [
        ["(?=(a+))\\b\\1", "", "aa", 0, ["aa", "aa"]],
        ["(?=(a+))\\b\\1", "", "baa", null, null],
        ["(?=(a+))^\\1", "", "baa", null, null],
        ["(?=(a+))(?=a)\\1b", "", "aab", 0, ["aab", "aa"]],
        ["(?=(a+))(?!b)\\1b", "", "aab", 0, ["aab", "aa"]],
        ["(?=(a+))(?=(?:(b)|))\\1", "", "aa", 0, ["aa", "aa", null]],
        ["(?=(ab?))(?=(a))\\1", "", "ab", 0, ["ab", "ab", "a"]],
        ["(?=(a))(?=(ab?))\\1", "", "ab", 0, ["a", "a", "ab"]],
    ],
    groupBoundariesBetween: [
        ["(?:(?=(a+)))\\1b", "", "aab", 0, ["aab", "aa"]],
        ["(?=(a+))(?:\\1)b", "", "aab", 0, ["aab", "aa"]],
        ["(?=(a+))(?:\\1b)", "", "aab", 0, ["aab", "aa"]],
        ["(?:(?:(?=(a+))))(?:(?:\\1))b", "", "aab", 0, ["aab", "aa"]],
        ["((?=(a+)))\\2b", "", "aab", 0, ["aab", "", "aa"]],
        ["(?=(a+))(\\1)b", "", "aab", 0, ["aab", "aa", "aa"]],
        ["(?=(a))(?:\\1|b)", "", "ab", 0, ["a", "a"]],
    ],
    lookaheadNestedAtLimit: [
        [nested("(?=(a+))", maxGroupsCrossedToRepeatedLookahead) + "\\1b", "", "aab", 0, ["aab", "aa"]],
    ],
    referenceNested: [
        ["(?=(a+))" + nested("\\1", maxGroupsCrossedToRepeatedLookahead) + "b", "", "aab", 0, ["aab", "aa"]],
        ["(?=(a+))" + nested("\\1", maxGroupsCrossedToRepeatedLookahead) + "b", "", "aa", null, null],
        ["(?=(a+))" + nested("\\1", maxGroupsCrossedToRepeatedLookahead + 1) + "b", "", "aab", 0, ["aab", "aa"]],
        ["(?=(a+))" + nested("\\1", maxGroupsCrossedToRepeatedLookahead + 1) + "b", "", "aa", null, null],
    ],
    failureAfterReference: [
        ["(?:(?=(a+))\\1b|a+c)", "", "aac", 0, ["aac", null]],
        ["(?:(?=(a+))\\1,)*", "", "a,aa,aaa", 0, ["a,aa,", "aa"]],
        ["(?:(?=(a+))\\1b)?a", "", "aac", 0, ["a", null]],
        ["(?=(a+))\\1b|(a)", "", "aac", 0, ["a", null, "a"]],
        ["(x)?(?=(a+))\\2b|a", "", "xaa", 1, ["a", null, null]],
    ],
};

const mustCompare = {
    consumingTermBetween: [
        ["(?=(a))a\\1", "", "aa", 0, ["aa", "a"]],
        ["(?=(a))a\\1", "", "ab", null, null],
        ["(?=(a+))a\\1", "", "aaa", null, null],
        ["(?=(a+))a*\\1", "", "aaaa", 0, ["aaaa", "aaaa"]],
        ["(?=(a+))(?:b|)\\1", "", "aa", 0, ["aa", "aa"]],
        ["(?=(a+))\\1\\1", "", "aaaa", null, null],
        ["(?=(a(b)?))\\1\\2", "", "abb", 0, ["abb", "ab", "b"]],
        ["(?=(a(b)?))\\1\\2", "", "aa", 0, ["a", "a", null]],
    ],
    partialLookaheadCapture: [
        ["(?=(a)b)\\1", "", "ab", 0, ["a", "a"]],
        ["(?=.(a))\\1", "", "aa", 0, ["a", "a"]],
        ["(?=.(a))\\1", "", "ba", null, null],
        ["(?=x(a))\\1", "", "xa", null, null],
        ["(?=((a)b?))\\2", "", "ab", 0, ["a", "ab", "a"]],
    ],
    quantifiedCapture: [
        ["(?=(a){2})\\1", "", "aa", 0, ["a", "a"]],
        ["(?=(a)+)\\1", "", "aaa", 0, ["a", "a"]],
        ["(?=(a)?)\\1", "", "b", 0, ["", null]],
    ],
    possiblyUnsetCapture: [
        ["(?=(a)|b)\\1", "", "b", 0, ["", null]],
        ["(?!(a))\\1", "", "b", 0, ["", null]],
        ["(?!(a))\\1", "", "a", 1, ["", null]],
        ["(?:(?=(a))|b)\\1", "", "ba", 0, ["b", null]],
    ],
    optionalQuantifiedLookahead: [
        ["(?=(a))?\\1", "", "a", 0, ["", null]],
        ["(?=(a))*\\1", "", "a", 0, ["", null]],
        ["(?=(a)){0}\\1", "", "a", 0, ["", null]],
    ],
    lookaheadInQuantifiedGroup: [
        ["(?:(?=(a)))+\\1", "", "aa", 0, ["a", "a"]],
    ],
    referenceToOtherGroup: [
        ["(a)(?=(b))\\1", "", "aba", null, null],
    ],
    quantifiedReference: [
        ["(?=(a))\\1*", "", "aaab", 0, ["aaa", "a"]],
        ["(?=(a))\\1{2}", "", "aaa", 0, ["aa", "a"]],
        ["(?=(a))\\1{2}", "", "ab", null, null],
        ["(?=(a))\\1?", "", "aa", 0, ["a", "a"]],
        ["(?=(a))(?:\\1){2}", "", "aa", 0, ["aa", "a"]],
        ["(?=(a))(?:\\1){2}", "", "ab", null, null],
        ["(?=(a))(?:\\1)+", "", "aaab", 0, ["aaa", "a"]],
    ],
    lookaheadNestedPastLimit: [
        [nested("(?=(a+))", maxGroupsCrossedToRepeatedLookahead + 1) + "\\1b", "", "aab", 0, ["aab", "aa"]],
    ],
};

const unsupportedByReference = {
    duplicateNames: [
        ["(?:(?=(?<x>a))|(?<x>b))\\k<x>", "", "a", 0, ["a", "a", null]],
        ["(?:(?=(?<x>a))|(?<x>b))\\k<x>", "", "bb", 0, ["bb", null, "b"]],
    ],
    surrogatePairs: [
        ["(?=(\\u{1f600}+))\\1x", "u", "\ud83d\ude00\ud83d\ude00x", 0, ["\ud83d\ude00\ud83d\ude00x", "\ud83d\ude00\ud83d\ude00"]],
        ["(?=(\\u{1f600}))\\1{2}", "u", "\ud83d\ude00\ud83d\ude00", 0, ["\ud83d\ude00\ud83d\ude00", "\ud83d\ude00"]],
    ],
};

for (const cases of [...Object.values(skipsComparison), ...Object.values(mustCompare)])
    checkCases(cases);
for (const cases of Object.values(unsupportedByReference))
    checkCases(cases, { skipReference: true });

const lookaheads = ["(?=(?<x>C))", "(?=(?<x>C)b)", "(?=b?(?<x>C))", "(?=(?<x>C)|b)", "(?=(?<x>C)+)", "(?=(?<x>C)?)", "(?!(?<x>C))", "(?=(?:(?<x>C)))", "(?:(?=(?<x>C)))", "(?:(?=(?<x>C)))?"];
const contents = ["a", "a+", "ab?"];
const middles = ["", "\\b", "^", "(?=a)", "(?!b)", "a", "a?"];
const references = ["\\k<x>", "\\k<x>*", "\\k<x>{2}", "(?:\\k<x>)", "(?:\\k<x>|b)"];
const tails = ["", "b", "a"];
const stringsOverAB = stringsOver("ab", 4);

for (const lookahead of lookaheads) {
    for (const content of contents) {
        for (const middle of middles) {
            for (const reference of references) {
                for (const tail of tails) {
                    const source = lookahead.replace("C", content) + middle + reference + tail;
                    checkRegExp(source, "", stringsOverAB);
                    if (middle === "")
                        checkRegExp(source.replace("(?<x>", "(").replaceAll("\\k<x>", "\\1"), "", stringsOverAB);
                }
            }
        }
    }
}

const lookaheadsWithQuantifiedTerm = ["(?=a+)ab", "(?=a+b)a", "(?=[ab]+)b", "(?=(?:a+))a", "(?=a*)b", "(?!a+b)a", "(?!a*b)a", "(?=a+?)a", "(?=a{2,})a"];
const suffixes = ["", "(c)?", "(c)\\1", "(c)?\\1"];
const stringsOverABC = stringsOver("abc", 4);

for (const source of lookaheadsWithQuantifiedTerm) {
    for (const suffix of suffixes)
        checkRegExp(source + suffix, "", stringsOverABC);
}
