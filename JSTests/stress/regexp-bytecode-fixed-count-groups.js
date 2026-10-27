//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1", "--maxRegExpStackSize=65536")
//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")
//@ runDefault

// Duplicate named groups, a class with strings and a lookbehind make a pattern run as bytecode where there is no JIT. There a group that is repeated a fixed number of times and makes no choice
// keeps the contexts of its first and its last turn, so it takes any number of turns in 65536 bytes.
function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}

function describe(match) {
    return match === null ? "null" : match.index + ":" + match.map(part => part === undefined ? "-" : part.length).join(",");
}

const kinds = [
    // [what stands in front, what stands behind, flags, what it adds to the description]
    ["(?:(?<n>x)|(?<n>y))?", "", "", captures => ",-,-" + captures],
    ["", "(?:(?<n>x)|(?<n>y))?", "", captures => captures + ",-,-"],
    ["", "[\\q{zz|z}]?", "v", captures => captures],
    ["(?<=^|q)", "", "", captures => captures],
];

const turns = 20000;
const manyTurns = [
    // [pattern with N for the count, one turn, length and captures with every turn there, the same with one turn missing]
    ["(?:(?:ab){N}|a)", "ab", [2 * turns, ""], [1, ""]],
    ["(?:(?:ab){N}c|a)", "ab", [1, ""], [1, ""]],
    ["(?:(ab){N}|a)", "ab", [2 * turns, ",2"], [1, ",-"]],
    ["(?:(?:(a)(b)){N}|a)", "ab", [2 * turns, ",1,1"], [1, ",-,-"]],
    ["(?:(?:(?:ab){2}){N}|a)", "abab", [4 * turns, ""], [1, ""]],
    ["(?:(?:(?:(a)b){2}){N}|a)", "abab", [4 * turns, ",1"], [1, ",-"]],
    ["(?:(?:[ab]c){N}|.)", "bc", [2 * turns, ""], [1, ""]],
    ["(?:(?:a(?=(b))b){N}|a)", "ab", [2 * turns, ",1"], [1, ",-"]],
    ["(?:(?=(?:ab){N})a|a)", "ab", [1, ""], [1, ""]],
    ["(?:(?:ab){N}(?:ab){N}|(?:ab){N}|a)", "ab", [2 * turns, ""], [1, ""]],
];
for (const [front, behind, flags, withCaptures] of kinds) {
    for (const [source, turn, whole, short] of manyTurns) {
        const expression = new RegExp(front + source.replaceAll("N", turns) + behind, flags);
        shouldBe(describe(expression.exec(turn.repeat(turns))), "0:" + whole[0] + withCaptures(whole[1]), expression.source + " on every turn");
        shouldBe(describe(expression.exec(turn.repeat(turns - 1))), "0:" + short[0] + withCaptures(short[1]), expression.source + " on one turn less");
    }
}

// A group without captures keeps no context, so it takes its turns in every turn of a group at the end of the pattern that is repeated without a bound.
for (const [source, turn] of [
    ["(?:(?<n>x)|(?<n>y))?(?:(?:ab){2})*", "abab"],
    ["(?:(?<n>x)|(?<n>y))?(?:c(?:ab){3})*", "cababab"],
    ["(?:(?<n>x)|(?<n>y))?(?:(?:(?:ab){2}c){2})*", "ababcababc"],
])
    shouldBe(describe(new RegExp(source).exec(turn.repeat(turns))), "0:" + turn.length * turns + ",-,-", source);

// What a group had captured before the turns began is there again behind turns that fail, and the text is read on from where the turns began.
const restored = [
    ["(?:(a)|b)(?:(?:(x)y){3}z|.*)", "axyxyxyw", "0:8,1,-"],
    ["(a)?(?:(?:(b)c){2}d|bcbc)", "abcbce", "0:5,1,-"],
    ["(?:(a)|(?:(b)c){2}d|bcbc)+", "abcbce", "0:5,-,-"],
    ["(?:(?:(a)b){2}c|(?:ab)+)\\1x", "ababx", "0:5,-"],
    ["(?:(?:(a)|(b)){1}(?:(c)(d)){2}e|.)+", "acdcdebcdcdf", "0:12,-,-,-,-"],
    ["^(?:(?:(\\w)-){2}(\\d)|(?:(\\w)-){3}x)", "a-b-c-x", "0:7,-,-,1"],
    ["(?:(?:(a)){3}b|a(a)?)", "aaac", "0:2,-,1"],
    ["(?=(?:(a)b){2}c|(a))ab", "ababd", "0:2,-,1"],
    ["(a*)(?:ab){2}b?", "aaababb", "0:7,2"],
    ["(.*)(?:ab){2}$", "ababab", "0:6,2"],
    ["(?:(?:ab){2}c)*", "ababcababcabc", "0:10"],
    ["(?:(?:ab){2}c)+?d", "ababcababcd", "0:11"],
    ["(?:(?:ab){2}|a|b)*c", "abababc", "0:7"],
    ["(?:(?:(?:a){2}){2}){2}", "aaaaaaaaa", "0:8"],
    ["(?:(?:(?:a){2}){2}){2}", "aaaaaaa", "null"],
    ["(?:\\b(?:ab){2}){2}", "abababab", "null"],
    ["(?:(?:ab){2}\\b ?){2}", "abab abab", "0:9"],
];
for (const [source, subject, expected] of restored) {
    shouldBe(describe(new RegExp(source + "(?:(?<n>!)|(?<n>\\?))?").exec(subject)), expected === "null" ? expected : expected + ",-,-", source + " before duplicate named groups");
    shouldBe(describe(new RegExp(source + "[\\q{zz|z}]?", "v").exec(subject)), expected, source + " before a class with strings");
}

// Behind the text, the turns are taken from right to left.
for (const [source, subject, expected] of [
    ["(?<=(?:ab){3})c", "abababc", "6:1"],
    ["(?<=(?:ab){3})c", "xbababc", "null"],
    ["(?<=(?:(a)(b)){3})c", "abababc", "6:1,1,1"],
    ["(?<=(?:(a)b){3}|(b))c", "xbababc", "6:1,-,1"],
    ["(?<=^(?:ab){" + turns + "})c", "ab".repeat(turns) + "c", 2 * turns + ":1"],
    ["(?<=^(?:ab){" + turns + "}|(b))c", "ab".repeat(turns - 1) + "c", 2 * turns - 2 + ":1,1"],
])
    shouldBe(describe(new RegExp(source).exec(subject)), expected, source);
