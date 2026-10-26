//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1", "--maxRegExpStackSize=65536")
//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")
//@ runDefault

// A group that is repeated a fixed number of times and makes no choice keeps no frame for a turn, so it takes any number of turns in 65536 bytes, which hold 4096 frames.
function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}

function describe(match) {
    return match === null ? "null" : match.index + ":" + match.map(part => part === undefined ? "-" : part.length).join(",");
}

const turns = 20000;
const manyTurns = [
    // [pattern with N for the count, one turn, what is found with every turn there, what is found with one turn missing]
    ["(?:ab){N}|a", "ab", "0:" + 2 * turns, "0:1"],
    ["(?:ab){N}c|a", "ab", "0:1", "0:1"],
    ["(ab){N}|a", "ab", "0:" + 2 * turns + ",2", "0:1,-"],
    ["(?:(a)(b)){N}|a", "ab", "0:" + 2 * turns + ",1,1", "0:1,-,-"],
    ["(?:(?:ab){2}){N}|a", "abab", "0:" + 4 * turns, "0:1"],
    ["(?:(?:(a)b){2}){N}|a", "abab", "0:" + 4 * turns + ",1", "0:1,-"],
    ["(?:[ab]c){N}|.", "bc", "0:" + 2 * turns, "0:1"],
    ["(?:a(?=b)b){N}|a", "ab", "0:" + 2 * turns, "0:1"],
    ["(?:a(?=(b))b){N}|a", "ab", "0:" + 2 * turns + ",1", "0:1,-"],
    ["(?:x|(?:ab){N})|a", "ab", "0:" + 2 * turns, "0:1"],
    ["(?=(?:ab){N})a|b", "ab", "0:1", "1:1"],
    ["(?!(?:ab){N})a|b", "ab", "1:1", "0:1"],
    ["(?:Жb){N}|Ж", "Жb", "0:" + 2 * turns, "0:1"],
];
for (const [source, turn, whole, short] of manyTurns) {
    const expression = new RegExp(source.replace("N", turns));
    shouldBe(describe(expression.exec(turn.repeat(turns))), whole, expression.source + " on every turn");
    shouldBe(describe(expression.exec(turn.repeat(turns - 1))), short, expression.source + " on one turn less");
}

// What a group had captured before the turns began is there again behind turns that fail.
const restored = [
    ["(?:(a)|b)(?:(?:\\1|(c)){0}(?:(x)y){3}z|.*)", "axyxyxyw", "0:8,1,-,-"],
    ["(a)?(?:(?:(b)c){2}d|bcbc)", "abcbce", "0:5,1,-"],
    ["(?:(a)|(?:(b)c){2}d|bcbc)+", "abcbce", "0:5,-,-"],
    ["(?:(?:(a)b){2}c|(?:ab)+)\\1x", "ababx", "0:5,-"],
    ["(?:(?:(a)|(b)){1}(?:(c)(d)){2}e|.)+", "acdcdebcdcdf", "0:12,-,-,-,-"],
    ["^(?:(?:(\\w)-){2}(\\d)|(?:(\\w)-){3}x)", "a-b-c-x", "0:7,-,-,1"],
    ["(?:(?:(a)){3}b|a(a)?)", "aaac", "0:2,-,1"],
    ["(?=(?:(a)b){2}c|(a))ab", "ababd", "0:2,-,1"],
];
for (const [source, subject, expected] of restored)
    shouldBe(describe(new RegExp(source).exec(subject)), expected, source);

// The count of an inner group starts anew in each turn of an outer one, whichever way the outer one is repeated.
for (const [source, subject, expected] of [
    ["(?:(?:ab){2}c)*", "ababcababcabc", "0:10"],
    ["(?:(?:ab){2}c)+?d", "ababcababcd", "0:11"],
    ["(?:(?:ab){2}|a|b)*c", "abababc", "0:7"],
    ["(?:(?:(?:a){2}){2}){2}", "aaaaaaaaa", "0:8"],
    ["(?:(?:(?:a){2}){2}){2}", "aaaaaaa", "null"],
    ["(?:\\b(?:ab){2}){2}", "abababab", "null"],
    ["(?:(?:ab){2}\\b ?){2}", "abab abab", "0:9"],
])
    shouldBe(describe(new RegExp(source).exec(subject)), expected, source);
