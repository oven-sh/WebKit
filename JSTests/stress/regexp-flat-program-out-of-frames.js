//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1", "--maxRegExpStackSize=65536")

// A flat program keeps a frame for each turn of a repeated group. Where it may not have more (here: 4096), the bytecode finishes the match: running out of frames is no answer.
function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}

const maxFrames = 65536 / 16;
const groups = [
    ["x(?:ab)*", "ab"],
    ["x(?:ab)+", "ab"],
    ["x(?:ab|cd)*", "cd"],
    ["x(?:[ab]c)*", "bc"],
    ["x(?:Жb)*", "Жb"],
    ["x(?:[^\\u{10000}]b)*", "ab", "u"],
];
for (const [text, iteration, flags] of groups) {
    const expression = new RegExp(text, flags);
    for (const iterations of [1, maxFrames - 1, maxFrames, 3 * maxFrames, 2]) {
        for (const prefix of ["", "yy", "Ж"]) {
            const what = expression + " " + iterations + " " + prefix.length;
            const match = expression.exec(prefix + "x" + iteration.repeat(iterations) + "a");
            shouldBe(match === null, false, what);
            shouldBe(match.index, prefix.length, what);
            shouldBe(match[0].length, 1 + 2 * iterations, what);
            shouldBe(expression.test(prefix + "y" + iteration.repeat(iterations) + "a"), false, what);
        }
    }
}
