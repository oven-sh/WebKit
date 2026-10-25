//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")
//@ runDefault

// A flat program reads a lookbehind's groups, runs and references backward, keeps its table of failed searches where the pattern has references, and in unicode mode takes a surrogate pair as one
// character. The validation runs it beside the bytecode on every match, with and without the table, and stops where the results differ.
function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}
function show(match) {
    return match ? match.index + ":" + Array.from(match, group => group === undefined ? "-" : group).join("|") : "null";
}

const face = "\u{1f600}", heart = "\u{1f60d}", lead = "\ud83d", trail = "\ude00", deseret = "\u{10400}", smallDeseret = "\u{10428}";

const expectations = [
    [/(?<=a+)b/, "caab", "3:b"], [/(?<=(a+))b/, "caab", "3:b|aa"], [/(?<=(a+?))b/, "caab", "3:b|a"], [/(?<=c(?:a|b)*)d/, "cabd", "3:d"], [/(?<=(a|b)*)d/, "cabd", "3:d|a"],
    [/(?<=(\w)+)!/, "ab!", "2:!|a"], [/(?<=([ab]+)([bc]+))$/, "abc", "3:|a|bc"], [/(?<=\1(a))b/, "aab", "2:b|a"], [/(?<=(a)\1)b/, "aab", "2:b|a"], [/(?<=\1(a))b/, "cab", "null"],
    [/(?<!a+)b/, "aab b", "4:b"], [/(?<!(?:a|c)+)b/, "ab cb xb", "7:b"], [/(?<=^(?:a|b)+)c/, "abc", "2:c"], [/(?<=a(?=b)b*)c/, "abbc", "3:c"], [/(?<=(?<=a)b+)c/, "abbc", "3:c"],
    [/(?<=a{2,3})b/, "aaaab", "4:b"], [/(?<=(a{2,3}))b/, "aaaab", "4:b|aaa"], [/(?<=(a{10}))b/, "aaaaaaaaaaaab", "12:b|aaaaaaaaaa"], [/(?<=(?:(a)|(b))+)c/, "abc", "2:c|a|-"],
    [/(?<=(?:ab)+?)c/i, "ABabc", "4:c"], [/(?<=\b\w+)-/, "ab-", "2:-"], [/(?<=(a)?b)c/, "abc bc", "2:c|a"], [/(?<=(a)??b)c/, "abc", "2:c|-"],
    [/^.$/u, face, "0:" + face], [/^..$/u, face, "null"], [/^.$/u, lead, "0:" + lead], [/./gu, "a" + face, "0:a"], [/.$/u, "a" + face, "1:" + face], [/[^a]+/u, "a" + face + heart + "a", "1:" + face + heart],
    [new RegExp(face + "+", "u"), "a" + face + face + "b", "1:" + face + face], [new RegExp("^" + face + "{2}$", "u"), face + face, "0:" + face + face], [new RegExp("[" + face + "]", "u"), heart + face, "2:" + face],
    [/\ud83d/u, face, "null"], [/\ude00/u, face, "null"], [/\ud83d/u, lead + face, "0:" + lead], [/\ude00/u, face + trail, "2:" + trail], [/😀/u, face, "0:" + face], [/[\ud83d]/u, face, "null"],
    [/^.{2}$/u, face + heart, "0:" + face + heart], [/^.{1,2}?a/u, face + heart + "a", "0:" + face + heart + "a"], [/^(.+)(.)$/u, face + heart, "0:" + face + heart + "|" + face + "|" + heart],
    [/^(.*?)(.)$/u, face + heart, "0:" + face + heart + "|" + face + "|" + heart], [/(.)\1/u, face + face, "0:" + face + face + "|" + face], [/(.)\1/u, lead + face, "null"], [/(\ud83d)\1/u, lead + face, "null"],
    [/(.)\1/iu, deseret + smallDeseret, "0:" + deseret + smallDeseret + "|" + deseret], [/\u{10400}/iu, smallDeseret, "0:" + smallDeseret], [/(?<=.)a/u, face + "a", "2:a"], [/(?<=^.)a/u, face + "a", "2:a"],
    [/(?<=^..)a/u, face + "a", "null"], [/(?<=\ude00)a/u, face + "a", "null"], [/(?<=\ude00)a/u, trail + "a", "1:a"], [/(?<=(.+))a/u, face + heart + "a", "4:a|" + face + heart], [/(?<!\p{L}+)a/u, deseret + "a a", "4:a"],
    [/\P{L}/u, deseret + face, "2:" + face], [/\p{Lu}/u, smallDeseret + deseret, "2:" + deseret], [/[^\p{L}]$/v, "a" + face, "1:" + face], [/\b./u, face + "a", "2:a"], [/.\B/u, face + heart, "0:" + face],
    [/^(?:.|a)*$/u, face + "a" + lead, "0:" + face + "a" + lead], [/(?:^|.)\ude00/u, face + "x" + trail, "2:x" + trail], [/.(?=.$)/u, "a" + face + heart, "1:" + face], [/.(?!.)/u, face + heart, "2:" + heart],
];
for (const [pattern, subject, expected] of expectations)
    shouldBe(show(new RegExp(pattern.source, pattern.flags).exec(subject)), expected, pattern + " on " + JSON.stringify(subject));

const patterns = [
    "(a)(?:\\1{2}|aa)*c|a+", "(a|a)*\\1c|a+", "(a)(?:\\1*?a|a)*c|a+", "(a)(b?)(?:\\1\\2|a)*c|a+", "^(a+)+\\1b$", "^(?:(a+)\\1)+b$", "(a*)(?:\\1a|b)*c", "(a)(b)(c)?(d)?(e)?(?:\\1|\\2|\\3|\\4|\\5|a)*f|a+",
    "(['\"])(?:(?!\\1).|a)*\\1c|.+", "^(?:(a)|(b))+\\1\\2$", "(?:(a+)|b)+?\\1c", "(?=(a+))(?:\\1|a)*b", "(?<x>a+)(?:\\k<x>|a)*b", "((a)|b)+\\2\\1c", "(a+)(?:b\\1)*?c",
    "a+(?<=c(?:a|a)*)|a+", "(?:a|a)*(?<=b+)c|a+", "(?:a|a)*(?<!(?:a|b)+)c|a+", "(?<=(?:a|a)*)c|a+", "(?<=(a+)+)b", "(?<=(?:a+b?)*)c", "(?<=^(?:a|ab)*)b", "(?<=(a*)*)b", "(?<=(?:a*?|b)*?)c",
    "(?<=(?:a{1,2}){2,4})b", "(?<=\\1(a+))b", "(?<=(a+)\\1)b", "(?<=(?:\\1a|(b))+)c", "(?<!(a)\\1+)b", "(?<=a(?=(b+))\\1)c", "(?<=(?<!b)a+)b", "(?<=\\s+|^)a", "(?<=[ab]+?)c", "(?<=a{9,})b", "(?<=(?:ab){9})c",
    "(?:.|a)*c|.+", "(?:.|.)*c|.+", "(?:[^x]|a)*c|.+", "^(?:.+)+$", "^(.+?)+b$", "(.)(?:\\1|.)*c|.+", ".+(?<=c(?:.|.)*)|.+", "(?<=(.)+)a", "(?<=.{2,})a", "(?<!.+?b)a", "^.{3,5}$", "^.{2}?", ".{2,}?a", "(.{2})+$",
];
const subjects = [
    "", "a", "b", "ab", "aab", "aaab", "aaaaaaaaaaaa", "aaaaaaaaaaaab", "aaaaaaaaaaaac", "caaaaaaaaaaa", "abababababc", "aabbaabbc", "'aaaaaaaa", "'aaaa'c", "a b  a", "ababababababababababc",
    face, face + "a", "a" + face, face.repeat(9), face.repeat(9) + "c", "c" + face.repeat(9), face + "a".repeat(9), "a".repeat(9) + face, lead + face, face + trail, trail + lead, lead.repeat(5), (face + "a" + lead).repeat(3),
];
let matches = 0;
for (const flags of ["", "i", "g", "s", "u", "iu", "v"]) {
    for (const source of patterns) {
        const pattern = new RegExp(source, flags);
        for (const subject of subjects) {
            for (const lastIndex of [0, 1, 2]) {
                pattern.lastIndex = lastIndex;
                if (pattern.exec(subject))
                    ++matches;
            }
        }
    }
}
shouldBe(matches > 1000, true, "matches");
