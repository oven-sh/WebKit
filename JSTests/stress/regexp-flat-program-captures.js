//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")
//@ runDefault("--useRegExpJIT=0")
//@ runDefault
//@ runDefault("--useExecutableAllocationFuzz=true", "--fireExecutableAllocationFuzzAtOrAfter=1")
//@ runDefault("--useExecutableAllocationFuzz=true", "--fireExecutableAllocationFuzzRandomly=true")

function describe(result) {
    if (result === null)
        return "null";
    return "[" + Array.from(result, capture => capture === undefined ? "undefined" : JSON.stringify(capture)).join(", ") + "]";
}

function check(group, [source, flags, , expected], subject) {
    const actual = describe(new RegExp(source, flags).exec(subject));
    if (actual !== describe(expected))
        throw new Error(group + ": /" + source + "/" + flags + " on " + JSON.stringify(subject) + " is " + actual + ", expected " + describe(expected));
}

const groups = {
    abandonedAlternative: [
        ["(?:(a)b|a)c", "", "ac", ["ac", undefined]],
        ["(?:(a+)b|(a+)c)", "", "aac", ["aac", undefined, "aa"]],
        ["(?:()b|a)c", "", "ac", ["ac", undefined]],
        ["(?:x()|xy)z", "", "xyz", ["xyz", undefined]],
        ["(a)|b", "", "b", ["b", undefined]],
        ["(a)x|.b", "", "aab", ["ab", undefined]],
        ["(?:(a)b|a)\\1c", "", "ac", ["ac", undefined]],
        ["(?:(A)b|a)c", "i", "aC", ["aC", undefined]],
    ],
    skippedOptionalGroup: [
        ["(a)?b", "", "b", ["b", undefined]],
        ["(a)?b", "", "ab", ["ab", "a"]],
        ["(a)??b", "", "ab", ["ab", "a"]],
        ["(?:(a)x)?a", "", "a", ["a", undefined]],
        ["(a(b)?c)?abd", "", "abd", ["abd", undefined, undefined]],
    ],
    lookahead: [
        ["(?!(a)b)a", "", "ac", ["a", undefined]],
        ["(?!(a)b)\\1a", "", "ac", ["a", undefined]],
        ["(?:(?!(a+)b)a|aab)", "", "aab", ["aab", undefined]],
        ["(?=(a))\\1b|ac", "", "ac", ["ac", undefined]],
        ["(?:(?=(a+)b)x|aab)", "", "aab", ["aab", undefined]],
        ["(?:(?=())x|a)", "", "a", ["a", undefined]],
        ["(?=(a))a", "", "a", ["a", "a"]],
    ],
    loop: [
        ["(?:(a)|b)*", "", "ab", ["ab", undefined]],
        ["(?:(a)|b){2}", "", "ab", ["ab", undefined]],
        ["((a)|(b))+", "", "ab", ["ab", "b", undefined, "b"]],
        ["(?:(a)|(b)|(c))*", "", "abc", ["abc", undefined, undefined, "c"]],
        ["(?:(a)|(ab))+c", "", "abc", ["abc", undefined, "ab"]],
        ["(?:(a)|(b))*b", "", "ab", ["ab", "a", undefined]],
        ["(a*)*", "", "aab", ["aa", "aa"]],
        ["(a*?)+b", "", "aab", ["aab", "a"]],
    ],
    emptyFrameStack: [
        ["(a)(b)c", "", "abc", ["abc", "a", "b"]],
        ["(a)(b)c", "", "abdabc", ["abc", "a", "b"]],
        ["(\\d+)-(\\d+)", "", "10-200", ["10-200", "10", "200"]],
        ["(\\d+)-(\\d+)", "", "1 22-333", ["22-333", "22", "333"]],
        ["^(\\w+)\\s(\\w+)$", "", "hello world", ["hello world", "hello", "world"]],
        ["^(\\w+)\\s(\\w+)$", "", "hello  world", null],
        ["()x", "", "x", ["x", ""]],
        ["()x", "", "ax", ["x", ""]],
    ],
    lazyRunBeforeCharacter: [
        ["a*?b", "", "aaab", ["aaab"]],
        ["a*?b", "", "aaacaab", ["aab"]],
        ["a*?b", "", "Жaab", ["aab"]],
        ["a*?b", "", "aЖab", ["ab"]],
        ["a*?b", "", "aabЖ", ["aab"]],
        ["(a*?)b", "", "aab", ["aab", "aa"]],
    ],
    lazyRunBeforeEnd: [
        ["a*?$", "", "aaa", ["aaa"]],
        ["a*?$", "", "aab", [""]],
        ["a*?$", "", "baa", ["aa"]],
        ["a*?$", "", "Жaa", ["aa"]],
        ["a*?$", "", "aЖa", ["a"]],
        ["a*?$", "", "aaЖ", [""]],
    ],
    lazyRunBeforeClass: [
        ["\\w*?\\d", "", "ab12", ["ab1"]],
        ["\\w*?\\d", "", "ab", null],
        ["\\w*?\\d", "", "Жab1", ["ab1"]],
        ["\\w*?\\d", "", "abЖ1", ["1"]],
        ["\\w*?\\d", "", "ab1Ж", ["ab1"]],
    ],
    invertedClassRun: [
        ["[^,]*,", "", "ab,cd", ["ab,"]],
        ["[^,]*,", "", "abc", null],
        ["[^,]*,", "", ",", [","]],
        ["[^,]*,", "", "Жab,", ["Жab,"]],
        ["[^,]*,", "", "aЖb,", ["aЖb,"]],
        ["[^,]*,", "", "ab,Ж", ["ab,"]],
        ["[^,]*,", "", "abЖ", null],
        ["([^,]*),", "", "aЖb,", ["aЖb,", "aЖb"]],
    ],
    greedyRunBeforeClass: [
        ["\\w+\\d", "", "ab12", ["ab12"]],
        ["\\w+\\d", "", "ab1c", ["ab1"]],
        ["\\w+\\d", "", "abc", null],
        ["\\w+\\d", "", "1", null],
        ["\\w+\\d", "", "Жa1", ["a1"]],
        ["\\w+\\d", "", "aЖb2", ["b2"]],
        ["\\w+\\d", "", "a1Жb2", ["a1"]],
        ["\\w+\\d", "", "ab12Ж", ["ab12"]],
        ["(\\w+)\\d", "", "ab12", ["ab12", "ab1"]],
    ],
    runBeforeWideCharacter: [
        ["a*[a\\u0416]", "", "aaa", ["aaa"]],
        ["a*[a\\u0416]", "", "aaЖ", ["aaЖ"]],
        ["a*?[a\\u0416]", "", "aaЖ", ["a"]],
        ["a*?[b\\u0416]", "", "aaЖ", ["aaЖ"]],
        ["a*\\u0416", "", "aab", null],
    ],
    registersPastInlineFrames: [
        ["(?=a)".repeat(300) + "a", "", "ba", ["a"]],
        ["(?=a)".repeat(300) + "b|a", "", "a", ["a"]],
    ],
};

for (const [group, rows] of Object.entries(groups)) {
    for (const row of rows) {
        const subject = row[2];
        check(group, row, subject);
        check(group + ", 16-bit subject", row, (subject + "Ж").slice(0, -1));
    }
}

const manyA = "a".repeat(18);
const spentStepBudget = [
    ["(a*)*b", "", "a".repeat(40), null],
    ["(a+)+c|(a+)b", "", manyA + "b", [manyA + "b", undefined, manyA]],
];
for (const row of spentStepBudget)
    check("spentStepBudget", row, row[2]);
