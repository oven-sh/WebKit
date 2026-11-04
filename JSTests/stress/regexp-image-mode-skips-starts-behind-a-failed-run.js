//@ runDefault("--useRegExpImageMode=1")
//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")
//@ runDefault

// Neither the code that a build makes for a pattern nor a flat program walks a run again from every place inside it. Once a run without an upper bound has failed at all its ends, the next attempt begins
// behind the starts that would only enter the same run; with several alternatives the run remembers how far it failed. A sticky pattern makes one attempt and has none of that, so
// the sticky form, asked at one place after the other, says what each pattern has to find.
function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}
const show = match => match && match.index + ":" + Array.from(match, group => group === undefined ? "-" : group).join(",");
function firstMatchByStickyForm(source, flags, subject) {
    const sticky = new RegExp(source, flags + "y");
    for (let start = 0; start <= subject.length; ++start) {
        sticky.lastIndex = start;
        const match = sticky.exec(subject);
        if (match)
            return match;
    }
    return null;
}

const prefixes = ["", "\\[", "a", "[ab]", "\\b", "^", "x.", "(x)", "(?:\\[)", "\\[[^\\]]*\\]", "a*x", "\\[(\\w*)\\W"];
const opens = [["", ""], ["(", ")"], ["(?:", ")"], ["(?=(", "))"], ["(?=", ")"], ["(?!", ")"], ["(?=(", "))\\1"], ["(?:", ")?"], ["(?:", "){2}"]];
const runs = [".*?", ".+?", ".*", ".+", "[^\\]]*", "[^\\]]+?", "\\w+", "\\w*?", "a*", "a+?", "\\s+", "[^]*?", ".{2,}", ".{2,}?", ".{0,3}", "ā*"];
const rests = ["#", "\\]\\(#", "\\]", "$", "b", "\\b#", "(?:c|#)", "(#)", "(?=#)", "(?<=a)#", ".*#", "\\s*=", ""];
const subjects = [
    "", "a", "#", "[a](", "[a](#", "[a]([a]([a](#", "[a]([a]([a](x", "[a](\n[a](#", "aaaa", "aaaa#", "aaab", "xaaxaab", "a a a a=", "   x", "x   ", "[][][]#", "[a][b](#",
    "ab ab ab#", "[a](".repeat(12) + "x", "[a](".repeat(12) + "#", "aā[āa](#", "āāā#", "xa]xa](#",
];
let matches = 0;
for (const prefix of prefixes) {
    for (const [open, close] of opens) {
        for (const run of runs) {
            for (const rest of rests) {
                const one = prefix + open + run + rest + close;
                const sources = [one, prefix + open + run + close + rest, one + "|" + prefix + run + "!", "q|" + one, "^z|" + one];
                const source = sources[(prefix.length + open.length + run.length + rest.length) % sources.length];
                for (const flags of ["", "m"]) {
                    const pattern = new RegExp(source, flags);
                    for (const subject of subjects) {
                        const match = pattern.exec(subject);
                        matches += match !== null;
                        shouldBe(show(match), show(firstMatchByStickyForm(source, flags, subject)), "/" + source + "/" + flags + " on " + JSON.stringify(subject));
                    }
                }
            }
        }
    }
}
shouldBe(matches > 50000, true, "matches");

// A match behind many attempts that failed, each of which has ruled out starts.
const links = "[a](".repeat(2000);
shouldBe(show(/\[.+?\]\(#/.exec(links + "\n[b](#")), "8001:[b](#", "a lazy run");
shouldBe(show(/\[.*\]\(#/.exec(links + "\n[b](#")), "8001:[b](#", "a greedy run");
shouldBe(show(/\[.+?\]\(#/.exec(links + "[b](#")), "0:" + links + "[b](#", "a lazy run that reaches the end");
shouldBe(show(/\[(?=(.+?\]\(#))\1|\[(?=(.+?\]\(!))\2/.exec(links + "\n[b](!")), "8001:[b](!,-,b](!", "two emulated atomic groups");
shouldBe(show(/\[[^\]]*\]\(#/.exec(links + "[b](#")), "8000:[b](#", "a run that never goes back");
shouldBe(/\[.+?\]\(#/.exec(links), null, "no match, lazy");
shouldBe(/\[.*\]\(#/.exec(links), null, "no match, greedy");
shouldBe(/x*y/.exec("x".repeat(8000)), null, "no match from the first unit on");
shouldBe(show(/x*y/.exec("x".repeat(8000) + "zy")), "8001:y", "a run that takes nothing");
shouldBe("a  b   c".replace(/\s+(?=c)/g, "_"), "a  b_c", "every match of a global pattern");
shouldBe(/\[[^\]]*\].*?#/.exec(links), null, "no match, a run behind a run that never goes back");
shouldBe(show(/\[[^\]]*\].*?#/.exec(links + "\n[b](#")), "8001:[b](#", "a run behind a run that never goes back");
shouldBe(show(new RegExp("\\[[^\\]]*\\][^#\\n]*#").exec(links + "\n[b](#")), "8001:[b](#", "two runs that never go back");
shouldBe(show(new RegExp("\\w+ \\w+ .*?!").exec("a b ".repeat(500) + "\nc d e!")), "2001:c d e!", "three runs");
