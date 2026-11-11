//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")
//@ runDefault("--useRegExpImageMode=1")
//@ runDefault

// A run in front of a back reference. The flat program knows what the reference begins with where the group is sure to be set, keeps no undo frame for a capture that is set again before
// anything reads it, and hands a long search to a second program that goes from one possible end of the run to the next. Validation compares every result of both with the bytecode interpreter.
function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}
const show = match => match && match.index + ":" + Array.from(match, group => group === undefined ? "-" : group).join(",");

shouldBe(show(/(["'])(.*?)\1/.exec(`say "it's" now`)), `4:"it's",",it's`, "a quoted word");
shouldBe(show(/(["'])(.*?)\1/.exec(`say "" now`)), `4:"",",`, "an empty quote");
shouldBe(show(/(["'])(.*?)\1/.exec(`say "it's`)), null, "no closing quote");
shouldBe(show(/(["']).*\1/.exec(`a "b" c "d" e`)), `2:"b" c "d","`, "a greedy quote");
shouldBe(show(/^(.+)\1$/.exec("abcabc")), "0:abcabc,abc", "a doubled word");
shouldBe(show(/^(.*)\1$/.exec("")), "0:,", "nothing, doubled");
shouldBe(show(/^(.*?),\1$/.exec("left,left")), "0:left,left,left", "the same on both sides");
shouldBe(show(/\[(.+?)\]\(\1\)/.exec("[a](b) [b](b) [c](c)")), "7:[b](b),b", "a link to itself");
shouldBe(show(/\[(.+)\]\(\1\)/.exec("[a](b) [b](b) [c](d)")), "7:[b](b),b", "a link to itself, greedy");
shouldBe(show(/(a)?x.*?\1y/.exec("x__y")), "0:x__y,-", "a group that is not set");
shouldBe(show(/(a*)x.*?\1y/.exec("x__y")), "0:x__y,", "a group that is empty");
shouldBe(show(/(.*?)\1b/.exec("aab")), "0:aab,a", "a group that may be empty");
shouldBe(show(/(.*)\1b/.exec("xb")), "1:b,", "a greedy group that ends up empty");
shouldBe(show(/(a.*?\1)b/.exec("a_b")), "0:a_b,a_", "a reference inside its own group");
shouldBe(show(/(ā).*?\1/.exec("āxxā")), "0:āxxā,ā", "16 bits");
shouldBe(show(/(x)[^]*?ā\1/.exec("x\nāyāx")), "0:x\nāyāx,x", "a wide character between the run and the reference");

const heads = ["", "x", "^", "\\b", "[xy]", "(?:x)"];
const groups = ["(a)", "([ab])", "(ab)", "(a|b)", "(a?)", "(a)?", "(.)", "(\\w\\w)", "()", "(ā)"];
const runs = [".*?", ".+?", ".*", ".+", "[^b]*?", "[ab]+", "\\w{2,}?", "[^]{2,}", "(.*?)", "(.+)", "(?:.+?)", "((.)*?)", "(x.*?)", "(.+?x)"];
const betweens = ["", "x", "-x", "\\b", "[xy]", "$"];
const references = ["\\1", "\\2", "\\1\\1"];
const tails = ["", "#", "$", "(b)", ".*?\\1"];
const subjects = ["", "a", "aa", "aba", "abab", "axa", "xaxxa", "xa-xa#", "ab-xab", "xab__ab#", "a\na", "xaāaāx", "āxā", "aaaaaaaab", "xbaab-xb", "ababababab#"];
let matches = 0;
for (const head of heads) {
    for (const group of groups) {
        for (const run of runs) {
            for (const between of betweens) {
                const reference = references[(head.length + group.length + run.length + between.length) % references.length];
                const tail = tails[(group.length * 3 + run.length + between.length) % tails.length];
                for (const flags of ["", "ms"]) {
                    const pattern = new RegExp(head + group + run + between + reference + tail, flags);
                    for (const subject of subjects)
                        matches += pattern.exec(subject) !== null;
                }
            }
        }
    }
}
shouldBe(matches > 5000, true, "matches");

// Searches long enough to be handed over. Without that, and with a frame for each end of a run, these take seconds in the checking mode.
const links = "[a](".repeat(300);
for (const [source, subject, expected] of [
    ["\\[(.+?)\\]\\(\\1#", links + "x", null],
    ["\\[(.+)\\]\\(\\1#", links + "x", null],
    ["\\[(.+?)\\]\\(\\1#", links + "[b](b#", "1200:[b](b#,b"],
    ["\\[(.+)\\]\\(\\1#", links + "[b](b#", "1200:[b](b#,b"],
    ["([\"'])(.*?)\\1!", "'a\"b".repeat(300), null],
    ["([\"'])(.*?)\\1!", "'a\"b".repeat(300) + "'!", "0:" + "'a\"b".repeat(300) + "'!,'," + "a\"b'".repeat(299) + "a\"b"],
    ["(ā)(.*?)-\\1!", "ā-b".repeat(300), null],
]) {
    shouldBe(show(new RegExp(source).exec(subject)), expected, source);
}
