//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")
//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1", "--maxRegExpStackSize=65536")

// Where there is no JIT, a group that is repeated a fixed number of times, makes no choice and takes no input ends every turn as it ended the first: it makes one turn, whatever the count is.
// Duplicate named groups, a class with strings and a lookbehind make a pattern run as bytecode.
function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}

function describe(match) {
    return match === null ? "null" : match.index + ":" + match.map(part => part === undefined ? "-" : part.length).join(",");
}

const kinds = [
    // [what stands in front, what stands behind, flags, what it adds to the description]
    ["", "", "", captures => captures],
    ["(?:(?<n>x)|(?<n>y))?", "", "", captures => ",-,-" + captures],
    ["", "(?:(?<n>x)|(?<n>y))?", "", captures => captures + ",-,-"],
    ["", "[\\q{zz|z}]?", "v", captures => captures],
    ["(?<=^|q)", "", "", captures => captures],
];

const forms = [
    // [pattern with N for the count, where it matches in "ab" behind (?<=^|q), where it matches otherwise, length, captures]
    ["(?:\\b){N}", 0, 0, 0, ""],
    ["(?:){N}a", 0, 0, 1, ""],
    ["(?:){N}b", null, 1, 1, ""],
    ["(?:^){N}", 0, 0, 0, ""],
    ["(?:$){N}", null, 2, 0, ""],
    ["(?:\\B){N}", null, 1, 0, ""],
    ["(?:(?=a)){N}", 0, 0, 0, ""],
    ["(?:(?=(a))){N}", 0, 0, 0, ",1"],
    ["(?:(?!b)){N}", 0, 0, 0, ""],
    ["(?:(?<=a)){N}", null, 1, 0, ""],
    ["(\\b){N}", 0, 0, 0, ",0"],
    ["(?:()){N}", 0, 0, 0, ",0"],
    ["(?:(?:\\b){N}){N}", 0, 0, 0, ""],
    ["(?:(\\b){N}){N}", 0, 0, 0, ",0"],
    ["((?:\\b){N}){N}", 0, 0, 0, ",0"],
    ["(?:(?:(?:^){N}){N}){N}", 0, 0, 0, ""],
    ["(?:(?:(?=[ab])){N}[ab]){2}", 0, 0, 2, ""],
    ["(?:(?:(\\b){N}){N}(a)){1}", 0, 0, 1, ",0,1"],
    ["(?:(?:(?:\\B){N}){N}|a)", 0, 0, 1, ""],
    ["(?:(\\b){N}x|a)", 0, 0, 1, ",-"],
    ["(?:(?:(?=(a))){N}(?:(?=(b))){N}|ab)", 0, 0, 2, ",-,-"],
    ["(?:\\b){N}?a", 0, 0, 1, ""],
];

for (const count of [2, 3, 70000, 4000000000, 4294967294]) {
    for (const [front, behind, flags, withCaptures] of kinds) {
        for (const [source, indexAtStart, index, length, captures] of forms) {
            const expression = new RegExp(front + source.replaceAll("N", count) + behind, flags);
            const where = front.startsWith("(?<=") ? indexAtStart : index;
            shouldBe(describe(expression.exec("ab")), where === null ? "null" : where + ":" + length + withCaptures(captures), String(expression));
        }
    }
}
