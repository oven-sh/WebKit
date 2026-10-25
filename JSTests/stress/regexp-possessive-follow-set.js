//@ runDefault
//@ runDefault("--useRegExpImageMode=1")
//@ runDefault("--useRegExpJIT=0")
//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")

load("./resources/regexp-reference-matcher.js", "caller relative");

function shouldBe(actual, expected) {
    if (JSON.stringify(actual) !== JSON.stringify(expected))
        throw new Error(JSON.stringify(actual) + " instead of " + JSON.stringify(expected));
}

const rules = {
    nextCharacter: [
        ["a*b", "", "aaab", 0, ["aaab"]],
        ["a*b", "", "aaac", null, null],
        ["a*ab", "", "aaab", 0, ["aaab"]],
        ["a*a", "", "aaa", 0, ["aaa"]],
    ],
    classThenCharacter: [
        ["[a-c]*d", "", "abcd", 0, ["abcd"]],
        ["[a-d]*d", "", "abcd", 0, ["abcd"]],
    ],
    classThenClass: [
        ["\\d+[a-z]", "", "12a", 0, ["12a"]],
        ["\\w+[a-z]", "", "12a", 0, ["12a"]],
    ],
    invertedClass: [
        ["[^a]*a", "", "bba", 0, ["bba"]],
        ["[^a]*b", "", "bbb", 0, ["bbb"]],
    ],
    optionalFollower: [
        ["a*b?c", "", "aac", 0, ["aac"]],
        ["a*b?a", "", "aaa", 0, ["aaa"]],
    ],
    repeatedFollower: [
        ["a*b*c", "", "aabc", 0, ["aabc"]],
        ["a*b*a", "", "aa", 0, ["aa"]],
    ],
    zeroCountFollower: [
        ["a*a{0}b", "", "aab", 0, ["aab"]],
        ["a*b{0}a", "", "aa", 0, ["aa"]],
    ],
    groupFollower: [
        ["a*(?:b|c)d", "", "aacd", 0, ["aacd"]],
        ["a*(?:b|a)d", "", "aaad", 0, ["aaad"]],
    ],
    optionalGroupFollower: [
        ["a*(?:b)?c", "", "aac", 0, ["aac"]],
        ["a*(?:b)?a", "", "aa", 0, ["aa"]],
    ],
    emptyGroupFollower: [
        ["x*(?:|||)y", "", "xxy", 0, ["xxy"]],
        ["x*(?:|||)x", "", "xx", 0, ["xx"]],
    ],
    endOfGroup: [
        ["(?:xa*)y", "", "xaay", 0, ["xaay"]],
        ["(?:xa*)ay", "", "xaay", 0, ["xaay"]],
    ],
    endOfNestedGroups: [
        ["(?:(?:xa*)y?)z", "", "xaaz", 0, ["xaaz"]],
        ["(?:(?:xa*)y?)az", "", "xaaz", 0, ["xaaz"]],
    ],
    ownGroupNextIteration: [
        ["(?:x[ab]*){2}c", "", "xaxbc", 0, ["xaxbc"]],
        ["(?:a[ab]*){2}c", "", "aac", 0, ["aac"]],
    ],
    endOfPattern: [
        ["a*", "", "aaa", 0, ["aaa"]],
        ["xa*", "", "xaa", 0, ["xaa"]],
        ["xa*(?=a)", "", "xaa", 0, ["xa"]],
        ["a*(?=a)", "", "aaa", 0, ["aa"]],
    ],
    lookaheadThenCharacter: [
        ["a*(?=b)b", "", "aaab", 0, ["aaab"]],
        ["a*(?=b)b", "", "aaa", null, null],
        ["a*(?=ab)a", "", "aaab", 0, ["aaa"]],
    ],
    possiblyEmptyLookahead: [
        ["a*(?=b|)", "", "aab", 0, ["aa"]],
        ["a*(?=b|)a", "", "aaa", 0, ["aaa"]],
    ],
    negativeLookaheadAtEnd: [
        ["a*(?!a)", "", "aaa", 0, ["aaa"]],
        ["a*(?!$)", "", "aaa", 0, ["aa"]],
        ["a*(?!b)", "", "aab", 0, ["a"]],
    ],
    endOfInput: [
        ["a*$", "", "baa", 1, ["aa"]],
    ],
    endOfLine: [
        ["a*$", "m", "aa\na", 0, ["aa"]],
        ["a*$\\nb", "m", "aa\nb", 0, ["aa\nb"]],
        ["[a\\n]*$\\na", "m", "a\na", 0, ["a\na"]],
    ],
    wordBoundary: [
        ["a\\w*\\b\\w", "", "aab", null, null],
        ["\\w+\\b", "", "ab c", 0, ["ab"]],
        ["x\\w*\\b.", "", "xab c", 0, ["xab "]],
        ["x\\w*\\b.", "", "xab", null, null],
        ["-\\w*\\b", "", "-ab", 0, ["-ab"]],
        ["-\\w*\\b", "", "-", null, null],
        ["-\\w*\\b\\w", "", "-ab", 0, ["-a"]],
        ["\\w*\\b\\w", "", "ab", 0, ["a"]],
        ["a\\w*\\B\\w", "", "aab", 0, ["aab"]],
        ["\\w+\\B.", "", "ab", 0, ["ab"]],
    ],
    ignoreCase: [
        ["a*b", "i", "AaB", 0, ["AaB"]],
        ["a*A", "i", "aaa", 0, ["aaa"]],
        ["\\u00e9*\\u00e9", "i", "\u00e9\u00c9", 0, ["\u00e9\u00c9"]],
    ],
    unicodeCaseFolding: [
        ["\\u017f*t", "iu", "\u017f\u017ft", 0, ["\u017f\u017ft"]],
        ["\\u017f*s", "iu", "\u017f\u017fs", 0, ["\u017f\u017fs"]],
        ["k*\\u212a", "iu", "kk", 0, ["kk"]],
    ],
    dot: [
        [".*\\n", "", "ab\n", 0, ["ab\n"]],
        [".*\\n", "s", "ab\n", 0, ["ab\n"]],
        [".*b", "", "abb", 0, ["abb"]],
    ],
    nonGreedy: [
        ["a*?b", "", "aab", 0, ["aab"]],
        ["(a*?)a", "", "aa", 0, ["a", ""]],
    ],
    group: [
        ["(?:ab)*c", "", "ababc", 0, ["ababc"]],
        ["(?:ab)*abc", "", "ababc", 0, ["ababc"]],
        ["(?:ab)*a", "", "abab", 0, ["aba"]],
        ["(?:ab)*a", "", "ababa", 0, ["ababa"]],
    ],
    capturingGroup: [
        ["(ab)*c", "", "ababc", 0, ["ababc", "ab"]],
        ["(ab)*abc", "", "ababc", 0, ["ababc", "ab"]],
    ],
    abandonedAlternative: [
        ["(?:(a)b|c)d", "", "abd", 0, ["abd", "a"]],
        ["(?:(a)b|a)c", "", "ac", 0, ["ac", null]],
    ],
    optionalGroup: [
        ["(?:ab)?c", "", "abc", 0, ["abc"]],
        ["(?:ab)?abc", "", "abc", 0, ["abc"]],
        ["(ab)?abc", "", "abc", 0, ["abc", null]],
    ],
    groupAtLeastOnce: [
        ["(?:ab)+c", "", "ababc", 0, ["ababc"]],
        ["(?:ab)+abc", "", "ababc", 0, ["ababc"]],
        ["(?:ab)+abc", "", "abc", null, null],
    ],
    groupWithInnerRun: [
        ["(?:a+b)*c", "", "aabac", 4, ["c"]],
        ["(?:a+b)*a+c", "", "aabaac", 0, ["aabaac"]],
        ["(?:a+b)*a+bc", "", "aabaabc", 0, ["aabaabc"]],
        ["(?:a*b)*abc", "", "ababc", 0, ["ababc"]],
    ],
    possiblyEmptyGroup: [
        ["(?:a?)*b", "", "aab", 0, ["aab"]],
        ["(?:a?)*a", "", "aa", 0, ["aa"]],
    ],
    backreferenceFollower: [
        ["(a)b*\\1", "", "abba", 0, ["abba", "a"]],
        ["(a)a*\\1", "", "aaa", 0, ["aaa", "a"]],
    ],
    lookaheadTail: [
        ["(?=(a*))a", "", "aaa", 0, ["a", "aaa"]],
        ["(?=(a*)a)a", "", "aaa", 0, ["a", "aa"]],
        ["(?=(a*))a(b)?\\2", "", "aaa", 0, ["a", "aaa", null]],
        ["(?=(a*)a)a(b)?\\2", "", "aaa", 0, ["a", "aa", null]],
        ["(?=a*)b", "", "aab", 2, ["b"]],
        ["(?!a*b)a", "", "aab", null, null],
        ["(?!a*b)a", "", "aac", 0, ["a"]],
    ],
};

const unsupportedByReference = {
    astralCharacters: [
        ["\\u{1F600}*\\u{1F601}", "u", "\u{1F600}\u{1F600}\u{1F601}", 0, ["\u{1F600}\u{1F600}\u{1F601}"]],
        ["[\\u{1F600}\\u{1F601}]*\\u{1F601}", "u", "\u{1F600}\u{1F601}", 0, ["\u{1F600}\u{1F601}"]],
        ["[\\ud83d\\ude00]*\\ude00", "", "\ud83d\ude00", 0, ["\ud83d\ude00"]],
    ],
    classWithStrings: [
        ["[\\q{ab}a]*c", "v", "aabc", 0, ["aabc"]],
        ["[\\q{ab}a]*ab", "v", "aab", 0, ["aab"]],
    ],
    lookbehind: [
        ["(?<=(a*)a)b", "", "aaab", 3, ["b", "aa"]],
        ["(?<=a(a*))b", "", "aaab", 3, ["b", "aa"]],
    ],
    legacyOctalEscape: [
        ["(?:\\2a|(b))+", "", "b", 0, ["b", "b"]],
        ["(?:\\2a|(b))+", "", "b\x02a", 0, ["b\x02a", null]],
    ],
    analysisLimit: [
        ["a*" + "b?".repeat(1000) + "c", "", "aac", 0, ["aac"]],
        ["a*" + "b?".repeat(1000) + "a", "", "aaa", 0, ["aaa"]],
        ["a*" + "(?:".repeat(100) + "a" + ")".repeat(100), "", "aa", 0, ["aa"]],
    ],
};

for (const cases of Object.values(rules)) {
    checkCases(cases);
    for (const [source, flags, subject] of cases)
        checkRegExp(source, flags, stringsOver([...new Set(subject)].join(""), 4));
}
for (const cases of Object.values(unsupportedByReference))
    checkCases(cases, { skipReference: true });

shouldBe(/(ab)*c/d.exec("ababc").indices[1], [2, 4]);
shouldBe(/(ab)*abc/d.exec("ababc").indices[1], [0, 2]);
