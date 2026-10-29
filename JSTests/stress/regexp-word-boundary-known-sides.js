//@ runDefault
//@ runDefault("--useRegExpImageMode=1")
//@ runDefault("--useRegExpJIT=0")
//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")

load("./resources/regexp-reference-matcher.js", "caller relative");

const cases = {
    bothSidesKnown: [
        ["a\\bb", "", "ab", null, null],
        ["a\\Bb", "", "ab", 0, ["ab"]],
        ["a\\b-", "", "a-", 0, ["a-"]],
        ["a\\B-", "", "a-", null, null],
        ["-\\b-", "", "--", null, null],
        ["-\\B-", "", "--", 0, ["--"]],
        ["-\\ba", "", "-a", 0, ["-a"]],
        ["-\\Ba", "", "-a", null, null],
        ["\\d\\b[a-z]", "", "1a", null, null],
        ["\\w\\B\\w", "", "_9", 0, ["_9"]],
        ["[+-]\\b\\d", "", "x-1", 1, ["-1"]],
        ["(?:ab|cd)\\b-", "", "cd-", 0, ["cd-"]],
        ["(?:ab)+\\bc", "", "ababc", null, null],
        ["(a)\\b(-)", "", "a-", 0, ["a-", "a", "-"]],
        ["(a)\\B(-)", "", "a-", null, null],
        ["\\w\\b$", "", "ab", 1, ["b"]],
        ["\\w\\b$", "", "a-", null, null],
        ["a(?=b)\\bb", "", "ab", null, null],
        ["a(?=-)\\b-", "", "a-", 0, ["a-"]],
        ["a(?=b)\\Bb", "", "ab", 0, ["ab"]],
    ],
    oneSideKnown: [
        ["a\\b", "", "a", 0, ["a"]],
        ["a\\b", "", "ab", null, null],
        ["a\\b", "", "aba", 2, ["a"]],
        ["a\\B", "", "ab", 0, ["a"]],
        ["a\\B", "", "a", null, null],
        ["-\\b", "", "-a", 0, ["-"]],
        ["-\\b", "", "-", null, null],
        ["-\\B", "", "-", 0, ["-"]],
        ["-\\B", "", "--a", 0, ["-"]],
        ["\\ba", "", "a", 0, ["a"]],
        ["\\ba", "", "ba", null, null],
        ["\\ba", "", "ba-a", 3, ["a"]],
        ["\\Ba", "", "a", null, null],
        ["\\Ba", "", "ba", 1, ["a"]],
        ["\\b-", "", "-", null, null],
        ["\\b-", "", "a-", 1, ["-"]],
        ["\\B-", "", "-", 0, ["-"]],
        ["\\B-", "", "a--", 2, ["-"]],
    ],
    neitherSideKnown: [
        ["[a-]\\b[a-]", "", "a-", 0, ["a-"]],
        ["[a-]\\b[a-]", "", "aa", null, null],
        ["[a-]\\B[a-]", "", "--", 0, ["--"]],
        ["a?\\bb", "", "b", 0, ["b"]],
        ["a?\\bb", "", "ab", null, null],
        ["x-?\\by", "", "xy", null, null],
        ["x-?\\by", "", "x-y", 0, ["x-y"]],
        ["(?:a|-)\\bx", "", "ax", null, null],
        ["(?:a|-)\\bx", "", "-x", 0, ["-x"]],
        ["(?:a|)\\b-", "", "a-", 0, ["a-"]],
        ["(?:a|)\\b-", "", "-", null, null],
        ["(?:a|)\\b-", "", "b-", 1, ["-"]],
        ["a\\b(?:b|-)", "", "a-", 0, ["a-"]],
        ["a\\b(?:|b)", "", "ab", null, null],
        ["a\\b(?:|b)", "", "a", 0, ["a"]],
        ["a*\\b-", "", "-", null, null],
        ["a*\\b-", "", "b-", 1, ["-"]],
        ["-*\\ba", "", "a", 0, ["a"]],
        ["-*\\ba", "", "ba", null, null],
        ["(?=a)\\ba", "", "ba", null, null],
        ["a(?!b)\\b", "", "a-", 0, ["a"]],
        ["(a)\\1\\b", "", "aaa", 1, ["aa", "a"]],
        ["(?:a|-)\\bb", "", "ab", null, null],
        ["(?:a|-)\\bb", "", "-b", 0, ["-b"]],
        ["(?:a|)\\bb", "", "b", 0, ["b"]],
        ["(?:a|)\\bb", "", "ab", null, null],
        ["(?:a|)\\bb", "", "-b", 1, ["b"]],
    ],
    termsReorderedByGenerator: [
        ["[ab]-\\bc", "", "a-c", 0, ["a-c"]],
        ["[ab]-\\Bc", "", "a-c", null, null],
        ["a\\b[-+]b", "", "a-b", 0, ["a-b"]],
        ["a\\B[-+]b", "", "a-b", null, null],
        ["[-+]a\\b-", "", "-a-", 0, ["-a-"]],
        ["-\\b[ab]-", "", "-a-", 0, ["-a-"]],
        ["[ab]-\\b[cd]e", "", "a-ce", 0, ["a-ce"]],
    ],
    // U+017F and U+212A are word characters only under i with u, where they fold to s and k.
    flagDependentWordClass: [
        ["a\\b\\u017f", "", "a\u017f", 0, ["a\u017f"]],
        ["a\\b\\u017f", "i", "a\u017f", 0, ["a\u017f"]],
        ["a\\b\\u017f", "u", "a\u017f", 0, ["a\u017f"]],
        ["a\\b\\u017f", "iu", "a\u017f", null, null],
        ["a\\B\\u017f", "i", "a\u017f", null, null],
        ["a\\B\\u017f", "iu", "a\u017f", 0, ["a\u017f"]],
        ["s\\b", "iu", "\u017f-", 0, ["\u017f"]],
        ["s\\b", "iu", "\u017f\u017f", 1, ["\u017f"]],
        ["s\\b", "iu", "s\u212a", null, null],
        ["s\\b", "i", "\u017f-", null, null],
        ["s\\b", "i", "S\u017f", 0, ["S"]],
        ["k\\B\\u212a", "iu", "k\u212a", 0, ["k\u212a"]],
        ["k\\B\\u212a", "u", "k\u212a", null, null],
        ["\\u212a\\b-", "iu", "k-", 0, ["k-"]],
        ["\\u212a\\b-", "u", "\u212a-", null, null],
        ["\\u212a\\B-", "u", "\u212a-", 0, ["\u212a-"]],
        ["-\\b\\u017f", "iu", "-s", 0, ["-s"]],
        ["-\\b\\u017f", "u", "-\u017f", null, null],
        ["\\W\\bs", "iu", "\u017fs", null, null],
        ["\\W\\bs", "iu", "-\u017f", 0, ["-\u017f"]],
        ["\\w\\b", "iu", "\u212aa-", 1, ["a"]],
        ["\\w\\b", "u", "\u017f", null, null],
        ["\\w\\b", "u", "a\u212a", 0, ["a"]],
        ["s\\b\\u017f", "iu", "s\u017f", null, null],
        ["s\\b\\u017f", "i", "s\u017f", 0, ["s\u017f"]],
        ["a\\b\\u00e9", "i", "a\u00e9", 0, ["a\u00e9"]],
        ["a\\B\\u00e9", "i", "a\u00e9", null, null],
        ["\\u00e9\\bb", "i", "\u00e9b", 0, ["\u00e9b"]],
    ],
    endOfLine: [
        ["a\\b$", "m", "a\nb", 0, ["a"]],
        ["a\\B$", "m", "a\n", null, null],
        ["-\\b$", "m", "-\n", null, null],
        ["-\\B$", "m", "-\na", 0, ["-"]],
    ],
};

const neighbors = ["", "a", "-", "[a-z]", "[-+]", "[a-]", "[^a]", "[^-]", "\\w", "\\W", ".", "a?", "-?", "a*", "-+", "(?:a|b)", "(?:a|-)", "(?:a|)", "(a)", "(?:-a)*", "(?=a)", "(?!a)", "[ab]-", "-[ab]"];
const caseFoldedNeighbors = ["", "s", "k", "\\u017f", "\\u212a", "\\w", "\\W", "-", "[a-z]", "[^a-z]", "."];
const characters = ["a", "-"];
const possiblyEmptyTerms = ["a?", "-?", "a*", "(?:a|)", "(?:-a)*", "(?=a)", "(?!a)"];
const groupSides = ["a", "-", "a-", "-a"];
const groupQuantifiers = ["", "?", "*", "+", "{2}"];

function checkNeighbors(previousNeighbors, nextNeighbors, flags, subjects) {
    for (const previous of previousNeighbors) {
        for (const boundary of ["\\b", "\\B"]) {
            for (const next of nextNeighbors)
                checkRegExp(previous + boundary + next, flags, subjects);
        }
    }
}

for (const group of Object.values(cases))
    checkCases(group);

for (const source of ["\\ba", "\\Ba", "\\b-", "\\B-", "\\b[a-]", "a\\b", "-\\B"])
    checkRegExp(source, "", stringsOver("a-", 4), { allStartIndexes: true });

checkNeighbors(neighbors, neighbors, "", stringsOver("a-b", 4));

const caseFoldedSubjects = stringsOver("s\u017fk\u212a-", 3);
for (const flags of ["", "i", "u", "iu"])
    checkNeighbors(caseFoldedNeighbors, caseFoldedNeighbors, flags, caseFoldedSubjects);

const subjects = stringsOver("a-", 5);
for (const term of possiblyEmptyTerms) {
    checkNeighbors(characters.map(character => character + term), ["", ...characters], "", subjects);
    checkNeighbors(["", ...characters], characters.map(character => term + character), "", subjects);
}

for (const quantifier of groupQuantifiers) {
    checkNeighbors(groupSides.map(side => "(?:" + side), groupSides.map(side => ")" + quantifier + side), "", subjects);
    checkNeighbors(groupSides.map(side => side + "(?:"), groupSides.map(side => side + ")" + quantifier), "", subjects);
}
