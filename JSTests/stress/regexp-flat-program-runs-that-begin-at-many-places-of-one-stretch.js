//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")
//@ runDefault("--useRegExpImageMode=1")
//@ runDefault

// A run without an upper bound that begins at many places of one stretch goes on from the same places again and again. The table of failed searches says where the whole rest of such a run has failed.
// Beside references a search depends only on the bounds that a reference can still read: none inside a lookahead that holds no reference. And what is entered when a search has failed is entered
// under the bounds that the groups held when it began. The validation runs the flat programs beside the bytecode on every match, with the table from the start, and stops where the results or the
// turns differ.
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
// The bytecode takes a time that doubles with each link of the subject over these.
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
shouldBe(new RegExp(lookaheads(4)).exec(links + "x"), null, "four lookaheads that find nothing");
shouldBe(new RegExp(lookaheads(4)).exec(links + "[b](&")[4], "a](".concat("[a](".repeat(299), "[b](&"), "the fourth lookahead finds its text");
shouldBe(new RegExp(lookaheads(12)).exec(links + "[b](<").index, 0, "the twelfth lookahead finds its text");
shouldBe(new RegExp("\\[(?=(.+?\\]\\(#))\\1").exec(links + "\n[b](#").index, 1201, "a line ends the run");
shouldBe(new RegExp("\\[.*\\]\\(#").exec(links + "x"), null, "as much as possible");
shouldBe(new RegExp("\\[(.+?)\\]\\(\\1#").exec(links + "a#").index, 1196, "a reference behind the run");
shouldBe(new RegExp("(?:(a)+?)\\1(c)").exec("aaaa"), null, "a group that is repeated as seldom as possible");
shouldBe(new RegExp("(?:(a)+?)\\1(c)").exec("aaaac").index, 0, "a group that is repeated as seldom as possible, and its match");
