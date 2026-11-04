//@ runDefault
//@ runDefault("--useRegExpImageMode=1")
//@ runDefault("--useRegExpJIT=0")
//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")

load("./resources/regexp-reference-matcher.js", "caller relative");

function shouldBe(actual, expected) {
    if (JSON.stringify(actual) !== JSON.stringify(expected))
        throw new Error(JSON.stringify(actual) + " instead of " + JSON.stringify(expected));
}

const groups = {
    specificationNotes: [
        ["(z)((a+)?(b+)?(c))*", "zaacbbbcac", 0, ["zaacbbbcac", "z", "ac", "a", null, "c"]],
        ["(a*)*", "b", 0, ["", null]],
        ["(a*)b\\1+", "baaaac", 0, ["b", ""]],
    ],
    alwaysEmptyBody: [
        ["()*", "", 0, ["", null]],
        ["()+", "", 0, ["", ""]],
        ["(){2}", "", 0, ["", ""]],
        ["(){0}", "", 0, ["", null]],
        ["()*?", "", 0, ["", null]],
        ["()+?", "", 0, ["", ""]],
        ["(){2,}x", "x", 0, ["x", ""]],
        ["(?:)*x", "x", 0, ["x"]],
        ["(?:){3}x", "x", 0, ["x"]],
        ["(){100}", "", 0, ["", ""]],
    ],
    possiblyEmptyBody: [
        ["(a*)+", "b", 0, ["", ""]],
        ["(a*)+", "aab", 0, ["aa", "aa"]],
        ["(a*){2}", "aab", 0, ["aa", ""]],
        ["(a*){2,3}", "aab", 0, ["aa", ""]],
        ["(a*)*?b", "aab", 0, ["aab", "aa"]],
        ["(a*?)*?b", "aab", 0, ["aab", "a"]],
        ["(a?)*b", "aab", 0, ["aab", "a"]],
        ["(a?)+b", "b", 0, ["b", ""]],
        ["(a?){3}b", "ab", 0, ["ab", ""]],
        ["(?:a?){2,}?b", "aaab", 0, ["aaab"]],
        ["(a*){1,}?b", "aab", 0, ["aab", "aa"]],
        ["(a??){2}b", "aab", 0, ["aab", "a"]],
    ],
    optionalOnce: [
        ["(a*)?", "b", 0, ["", null]],
        ["(a*)?b", "aab", 0, ["aab", "aa"]],
        ["(a*)??b", "b", 0, ["b", null]],
        ["(a?)?b", "b", 0, ["b", null]],
        ["(|a)?b", "ab", 0, ["ab", "a"]],
        ["(a*)??$", "", 0, ["", null]],
    ],
    variableMinimum: [
        ["(a*){1,2}b", "b", 0, ["b", ""]],
        ["(a*){2,}b", "ab", 0, ["ab", ""]],
        ["(a|){2,3}b", "ab", 0, ["ab", ""]],
        ["(?:a|){3,}?b", "aab", 0, ["aab"]],
    ],
    nestedGroups: [
        ["((a*)*)*", "aab", 0, ["aa", "aa", "aa"]],
        ["((a*)+)+", "b", 0, ["", "", ""]],
    ],
    emptyAlternative: [
        ["(?:|a)*b", "ab", 0, ["ab"]],
        ["(|a)+", "aa", 0, ["aa", "a"]],
        ["(a|)+", "aa", 0, ["aa", "a"]],
        ["(a|)+", "b", 0, ["", ""]],
        ["(?:a|())*b", "ab", 0, ["ab", null]],
        ["(?:()|a)+b", "ab", 0, ["ab", null]],
    ],
    assertionOnlyBody: [
        ["(?:(?=(a)))*a", "a", 0, ["a", null]],
        ["(?:(?=(a)))+a", "a", 0, ["a", "a"]],
        ["(?:\\b)*a", "a", 0, ["a"]],
        ["(?:\\b){2}a", "a", 0, ["a"]],
        ["(?:^)+a", "a", 0, ["a"]],
        ["(?:$)*", "", 0, [""]],
        ["(?:(?!b))*a", "a", 0, ["a"]],
    ],
    emptyBackreference: [
        ["(?:(a)|\\1)*b", "ab", 0, ["ab", "a"]],
        ["(?:\\1)*(a)", "a", 0, ["a", "a"]],
        ["(\\1)+a", "a", 0, ["a", ""]],
        ["(?:\\1a|(b))+", "ba", 0, ["ba", null]],
    ],
    capturesResetEachIteration: [
        ["(?:(a)|(b)){2}", "ab", 0, ["ab", null, "b"]],
        ["(?:(a)|b)+", "ab", 0, ["ab", null]],
        ["(?:(a)|(b))*", "ab", 0, ["ab", null, "b"]],
    ],
};

const unsupportedByReference = [
    ["(?<=(a*)*)b", "", "aab", 2, ["b", "aa"]],
    ["(?<=(a*)+)b", "", "b", 0, ["b", ""]],
];

const subjects = stringsOver("ab", 4);
for (const cases of Object.values(groups)) {
    for (const flags of ["", "i", "u", "v"]) {
        checkCases(cases.map(([source, subject, index, captures]) => [source, flags, subject, index, captures]));
        for (const [source] of cases)
            checkRegExp(source, flags, subjects);
    }
}
checkCases(unsupportedByReference, { skipReference: true });

shouldBe("aab".replace(/(a*)*/g, "[$1]"), "[aa][]b[]");
shouldBe("ab".split(/(?:)*/), ["a", "b"]);
shouldBe([..."aa".matchAll(/(a?)*?/g)].length, 3);

const sticky = /(a*)+/y;
sticky.lastIndex = 2;
const result = sticky.exec("aab");
shouldBe(result.index, 2);
shouldBe(result[0], "");
shouldBe(result[1], "");
