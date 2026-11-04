//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")

// Where a flat program gives up at the limit of its steps, the match is made again on a flat program that takes one off the limit for each turn of a group, as the JIT does.
// The validation runs that program beside the bytecode on every match, and stops where the two have not counted the same number of turns.
function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}

const patterns = [
    "(?:a|a)*c|a+", "(?:a|a)*?c|a+", "(?:a|a)+c|a+", "(?:a|aa)+b|c", "(a+)+b|c", "(a*)*c|a+", "(?:a||a)*c|a+", "(?:a|a){0,5}c|a+", "(?:a|a){2,5}?c|a+", "(?:a|a){4}c|a+", "(?:a|a){3,}c|a+",
    "(?:(?:ab){2}|abab)*c|(?:ab)+", "(?:(?:a){3}|aaa)*c|a+", "(?:(?:a|b)|a)*c|a+", "(?:(?:a)?a|aa)*c|a+", "(?:(?:a|a)*)*c|a+", "((a|a)*b?)*c|a+", "(?:(?:a|a)*?)*?c|a+", "(?:a*|b*|a)*c|a+", "(?:a*){2,}c|a+",
    "(a)(?:\\1{2}|aa)*c|a+", "(a)(?:\\1*?a|a)*c|a+", "(a|a)*\\1c|a+", "(?:(?=a)a|a)*c|a+", "(?:(?=(?:a|b)*)a|a)*c|a+", "(?!(?:a|a)*c)a+", "(?=(?:a|a)*c)a|a+",
    "(?:\\n|\\bk\\s+(?:\\w+=\\S*\\s+)*)end", "^x__(?:c?(a+)+c|c?a+b)$", "^a|(?:a|b)*c", "(?:^a|a)*c|a+", "(?:a\\b|a)*c|a+", "(?:\\b|^)*a", "x(?:ab|cd)*", "(?:ab)*c", "x?(?:ab)*c", "(?:[ab]|[ac])*d|a+", "(?:.|a)*c|a+",
    "(?:a+b?)*?c", "(?:\\w+\\s)*\\d", "(?:\\S*\\s)+=", "(?:a*?b)*c", "(?:ab?c?)*d", "(?:(a)|(b)|(ab))*c",
];
const subjects = ["", "a", "aaaa", "aaaaaaaa", "aaaaaaaac", "aaaaaab", "abababab", "ababababc", "aaabaaab", "k A=k A=k A=k\nno\nend", "k A=k B= C=k", "x__caaaaaab", "x__caaaaaa", "xababcdab", "zzzzzz", "ab ab ab 1", "a b c =", "aabbaabbc", "abcabcabd"];
let matches = 0;
for (const flags of ["", "i", "m", "u", "y"]) {
    for (const source of patterns) {
        const pattern = new RegExp(source, flags);
        for (const subject of subjects) {
            for (const text of [subject, subject + "ā"]) {
                pattern.lastIndex = 0;
                if (pattern.exec(text))
                    ++matches;
            }
        }
    }
}
shouldBe(matches > 1000, true, "matches");
shouldBe(/(?:a|aa)+b|c/.exec("aaaaaac").index, 6, "the alternative behind the group");
shouldBe(new RegExp("(?:\\n|\\bk\\s+(?:\\w+=\\S*\\s+)*)end").test("k A=k A=k\nno\nend"), true, "words");
