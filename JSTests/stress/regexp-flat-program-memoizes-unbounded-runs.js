//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")
//@ runDefault("--useRegExpImageMode=1")
//@ runDefault

// Unbounded runs that start at many positions of one stretch of matching characters, with and without backreferences. The flat program memoizes where the rest of such a run has failed, keys its
// entries only by the capture bounds that a backreference can still read, and records a failed search under the bounds it started with. Validation runs the flat programs next to the bytecode
// interpreter on every match, with memoization on from the start, and crashes if the results or the turn counts differ.
function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}

const lookaheads = count => Array.from("#!%&@~;:=,+<".slice(0, count), (character, index) => "\\[(?=(.+?\\]\\(" + character + "))\\" + (index + 1)).join("|");
const bodies = [
    lookaheads(1), lookaheads(2), lookaheads(4), lookaheads(8), lookaheads(9), lookaheads(12),
    "\\[(?=(.+?\\]\\(#))", "\\[(?=(.+\\]\\(#))\\1", "\\[(?=(.*?\\]\\(#))\\1x", "\\[(?!(.+?\\]\\(#))a", "\\[(?=(.)(.+?\\1\\())\\2", "(.)(?=.+?\\1x)", "(a|\\[)(?=(.+?)\\1#)\\2", "\\[(?=(?=(.+?\\]\\(#))\\1)",
    "\\[(.+?)\\]\\(\\1#", "\\[(.+)\\]\\(\\1#", "\\[(.*?)\\]\\(\\1\\]", "(\\[|a)(.+?)\\]\\(\\1#", "(?:\\[|a)(.+?)\\]\\(\\1#", "(['\\[]).*?\\1#", "(?:(\\[)|(a)).+?\\1\\2#",
    "\\[.+?\\]\\(#", "\\[.+\\]\\(#", "\\[.*?\\].*?#", "\\[.*\\].*#", "\\[.{2,}?\\]\\(#", "\\[.{2,}\\]\\(#", "\\[.{0,5}?\\]\\(#", "\\[[^\\n]*?\\]$", "\\[[^]*?\\]\\(#", "\\[[^]*\\]\\(#", "a.*?\\b#", ".*?#", ".*#", ".+?x$",
];
// The bytecode interpreter takes exponential time on these.
const bodiesOfRepeatedGroups = [
    "(?:\\[.+?\\]\\(#?)*x#", "(?:\\[.+?\\]\\()+#", "(?:\\[.+\\]\\()+?#", "(?:\\[.*?\\]\\(){2,3}#", "(?:\\[.*\\]\\(){2,}#", "(?:.*?\\])*#", "(?:.*\\])*?#", "(.*?)*#", "(.*)*#", "(?:a*?)*#", "(?:(?:\\[.+?\\])+\\()+#", "(?:.*?\\(|.*?\\[)+#",
    "(?:(a)+?)\\1(c)", "(?:(a)+?)\\1c", "(?:(a|\\[)+?)\\1c", "(?:(a)|\\[)+?\\1c", "(?:(a)+)\\1(c)", "(?:(a){1,3}?)\\1(c)", "(?:(a)??)\\1(c)", "(?:(a+?)+?)\\1c", "(?:(a)+?|(\\[)+?)\\1\\2c", "(?:(a)+?(?=\\1))+?c",
];
const subjects = [
    "", "x", "aaaa", "aaaac", "aaaaaaaaac", "[a](", "[a](#", "[a](".repeat(3) + "x", "[a](".repeat(12) + "x", "[a](".repeat(40) + "x", "[a](".repeat(12) + "#", "[a](".repeat(12) + "[a](%", "[a](".repeat(12) + "x#",
    ("[a](".repeat(5) + "\n").repeat(4), ("[a](".repeat(5) + "\n").repeat(4) + "[a](&", "[[[[[[[[[[[[]", "[a]([a]([aa]([a]([aaa](aaa#", "[a]](a]#[a](a]", "'a'b'c'd", "a[a[a[a[a#",
];
let matches = 0;
for (const flags of ["", "s", "i", "u", "g"]) {
    for (const body of [...bodies, ...bodiesOfRepeatedGroups]) {
        const pattern = new RegExp(body, flags);
        for (const subject of subjects) {
            if (subject.length > 16 && bodiesOfRepeatedGroups.includes(body))
                continue;
            for (const text of [subject, subject + "ā", "\u{1f600}" + subject]) {
                for (const lastIndex of [0, 2]) {
                    pattern.lastIndex = lastIndex;
                    if (pattern.exec(text))
                        ++matches;
                }
            }
        }
    }
}
shouldBe(matches > 2000, true, "matches");

const links = "[a](".repeat(300);
shouldBe(new RegExp(lookaheads(4)).exec(links + "x"), null, "four lookaheads without a match");
shouldBe(new RegExp(lookaheads(4)).exec(links + "[b](&")[4], "a](".concat("[a](".repeat(299), "[b](&"), "the fourth lookahead matches");
shouldBe(new RegExp(lookaheads(12)).exec(links + "[b](<").index, 0, "the twelfth lookahead matches");
shouldBe(new RegExp("\\[(?=(.+?\\]\\(#))\\1").exec(links + "\n[b](#").index, 1201, "a newline ends the run");
shouldBe(new RegExp("\\[.*\\]\\(#").exec(links + "x"), null, "a greedy run");
shouldBe(new RegExp("\\[(.+?)\\]\\(\\1#").exec(links + "a#").index, 1196, "a backreference after the run");
shouldBe(new RegExp("(?:(a)+?)\\1(c)").exec("aaaa"), null, "a lazy group without a match");
shouldBe(new RegExp("(?:(a)+?)\\1(c)").exec("aaaac").index, 0, "a lazy group with a match");
shouldBe(new RegExp(Array.from({ length: 40 }, (_, index) => "(" + String.fromCharCode(97 + index % 26) + ")?").join("")).exec("abc!")[0], "abc", "forty optional groups");
shouldBe(new RegExp("^(?:.*/)?(?:.*/)?(?:.*/)?(?:.*/)?end$").exec("a/".repeat(24) + "x"), null, "greedy runs in four optional groups");
