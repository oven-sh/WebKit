//@ runDefault
//@ runDefault("--useRegExpImageMode=1")
//@ runDefault("--useRegExpJIT=0")

load("./resources/regexp-reference-matcher.js", "caller relative");

const skipsComparison = {
    adjacent: [
        ["(?<=(?=(?=(b))\\1)b)", "", "ab", 2, ["", "b"]],
        ["(?<=(?=(?=(b))\\1)b)", "", "aa", null, null],
        ["(?<=(?=(?=(a))\\1)b)", "", "ab", null, null],
        ["(?<=(?=(?=(b))\\1)b)", "i", "aB", 2, ["", "B"]],
        ["(?<=(?=(?=(b))\\1)b)", "u", "ab", 2, ["", "b"]],
        ["(?<=(?=(?=(?<x>b))\\k<x>)b)", "", "ab", 2, ["", "b"]],
    ],
    termsAfterReference: [
        ["(?<=(?=(?=(b+))\\1c)b)", "", "abbc", 2, ["", "bb"]],
        ["(?<=(?=(?=(b+))\\1c)b)", "", "abbbc", 2, ["", "bbb"]],
        ["(?<=(?=(?=(b+))\\1c)b)", "", "abbd", null, null],
        ["(?<=(?=(?=(b+))\\1cd)b)", "", "abbc", null, null],
        ["(?<=(?=(?=(b+))\\1cd)b)", "", "abbcd", 2, ["", "bb"]],
        ["(?<=(?=(?=(b+))\\1$)b)", "", "abb", 2, ["", "bb"]],
    ],
    termsBeforeLookahead: [
        ["(?<=(?=b(?=(c+))\\1d)b)", "", "abccd", 2, ["", "cc"]],
        ["(?<=(?=b(?=(c+))\\1d)b)", "", "abcc", null, null],
    ],
    groupBoundariesBetween: [
        ["(?<=(?=(?:(?=(b+)))(?:\\1)c)b)", "", "abbc", 2, ["", "bb"]],
        ["(?<=(?=(?:(?=(b+))\\1)c)b)", "", "abbc", 2, ["", "bb"]],
        ["(?<=(?=(?:(?=(b+))\\1)c)b)", "", "abbd", null, null],
    ],
    failureAfterReference: [
        ["(?<=(?=(?=(b))\\1c|b)b)", "", "ab", 2, ["", null]],
        ["(?<=(?=(?=(b))\\1c|b)b)", "", "abc", 2, ["", "b"]],
        ["(?<=(?=(?:(?=(b))\\1c)?b)b)", "", "ab", 2, ["", null]],
    ],
    lookbehindInsideLookahead: [
        ["(?<=(?=b(?<=(?=(?=(b))\\1)b))b)", "", "ab", 2, ["", "b"]],
        ["(?<=(?=b(?<=(?=(?=(b))\\1c)b))b)", "", "ab", null, null],
    ],
    twoLookbehinds: [
        ["(?<=(?=(?=(b))\\1)b)(?<=(?=(?=(b))\\2c)b)", "", "abc", 2, ["", "b", "b"]],
        ["(?<=(?=(?=(b))\\1)b)(?<=(?=(?=(b))\\2c)b)", "", "abd", null, null],
    ],
};

const mustCompare = {
    consumingTermBetween: [
        ["(?<=(?=(?=(b))b\\1)b)", "", "abb", 2, ["", "b"]],
        ["(?<=(?=(?=(b))b\\1)b)", "", "ab", null, null],
        ["(?<=(?=(?=(b))b\\1)b)", "", "abc", null, null],
    ],
    partialLookaheadCapture: [
        ["(?<=(?=(?=(b)c)\\1)b)", "", "abc", 2, ["", "b"]],
        ["(?<=(?=(?=.(c))\\1)b)", "", "abc", null, null],
        ["(?<=(?=(?=.(b))\\1)b)", "", "abb", 2, ["", "b"]],
    ],
    referenceMatchedBackward: [
        ["(?<=\\1(?=(b))b)", "", "bb", 2, ["", "b"]],
        ["(?<=\\1(?=(b))b)", "", "ab", null, null],
    ],
    quantifiedReference: [
        ["(?<=(?=(?=(b))\\1{2})b)", "", "abb", 2, ["", "b"]],
        ["(?<=(?=(?=(b))\\1{2})b)", "", "ab", null, null],
    ],
};

for (const cases of [...Object.values(skipsComparison), ...Object.values(mustCompare)])
    checkCases(cases, { skipReference: true });
