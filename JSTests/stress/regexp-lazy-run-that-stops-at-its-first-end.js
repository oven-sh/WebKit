//@ runDefault("--useRegExpImageMode=1")
//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")
//@ runDefault

// Where a pattern is analyzed, .*?\] becomes [^\]]*\] if what stands behind the ] cannot fail there and match behind a later ]: nothing stands behind it, or another run that takes
// all that the first one takes. A run with an upper bound is left as it is written, so the same pattern with {0,9999}? for *? says what each pattern has to find.
function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}
const show = match => match && match.index + ":" + Array.from(match, group => group === undefined ? "-" : group).join(",");
const bounded = source => source.replaceAll("*?", "{0,9999}?").replaceAll("+?", "{1,9999}?");

const heads = ["", "\\[", "a", "(x)?", "^"];
const runs = [".*?", ".+?", "[^]*?", "[ab]*?", "\\w*?", "[^c]+?", "a*?", "(.*?)", "(?:\\w+?)", "((a)*?)"];
const ends = ["\\]", "b", "c", "[bc]", "#", "\\n", "B", "."];
const rests = [
    "", ".*?#", ".*#", ".+?#", "(.*?)#", "..*?#", "[^]*?#", "\\w*#", "[ab]*?#", "a.*?#", "$", "\\b", "(?=#)", ".*?(?:#|c)", ".*?#\\1", ".*?(?:ab)*#", ".*?\\].*?#", "(.*)", "(?:.*?#)?", "\\]", "[^].[^]*", "[^].[^]*#", ".[^][^]*#", "..[^]*#", "[^][^][^]*#",
];
const subjects = [
    "", "a", "]", "a]", "[a]", "[a]b#", "[a][b]#", "[a]\n[b]#", "ab]ab]#", "aab", "abcabc#", "abc\nabc#", "b#b", "a]]#", "[]#", "xa]b]c#", "aBb#", "[a](".repeat(6) + "#", "ab".repeat(8) + "]",
    "ā]ā#", "[ā]b#ā", "cccb#", "]a#]b", "a]a#\\", "bbbb", "####", "x]]\nZ", "x]]\n#", "a]b]\nc#",
];
let matches = 0;
for (const head of heads) {
    for (const run of runs) {
        for (const end of ends) {
            for (const rest of rests) {
                const source = head + run + end + rest;
                for (const flags of ["", "s", "i"]) {
                    const pattern = new RegExp(source, flags);
                    const asWritten = new RegExp(bounded(source), flags);
                    for (const subject of subjects) {
                        const match = pattern.exec(subject);
                        matches += match !== null;
                        shouldBe(show(match), show(asWritten.exec(subject)), "/" + source + "/" + flags + " on " + JSON.stringify(subject));
                    }
                }
            }
        }
    }
}
shouldBe(matches > 100000, true, "matches");

shouldBe(/^.*?\][^].[^]*/.test("x]]\nZ"), true, "a step that fails where a later end can pass it");
shouldBe(show(/.*?\][^].[^]*/.exec("x]]\nZ")), "0:x]]\nZ", "the same, with the start free");
shouldBe(show(/"(.*?)"/.exec('say "a" and "b"')), '4:"a",a', "a quoted word");
shouldBe(show(/\[(.*?)\].*?"(.*?)"/.exec('x [a] b [c] "d" "e"')), '2:[a] b [c] "d",a,d', "three lazy runs");
shouldBe(show(/\[.*?\]x/.exec("[a]y[b]x")), "0:[a]y[b]x", "a run that has to pass its first end");
shouldBe(show(/\[.*?\].*?#/.exec("[a]\n[b]c#")), "4:[b]c#", "the second run stops at the end of the line");
shouldBe(show(/\[[^]*?\].*?#/.exec("[a]\n[b]c#")), "0:[a]\n[b]c#", "the first run takes more than the second");
shouldBe(show(/<(.+?)>(.*?)<\/\1>/.exec("<a>x</b></a>")), "0:<a>x</b></a>,a,x</b>", "a reference behind the runs");
const links = "[a](".repeat(400);
shouldBe(/\[.*?\].*?#/.exec(links), null, "no match on a long line");
shouldBe(show(/\[.*?\].*?#/.exec(links + "#")), "0:" + links + "#", "a match that takes a long line");
