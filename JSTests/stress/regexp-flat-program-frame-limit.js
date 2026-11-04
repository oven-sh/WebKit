//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1", "--maxRegExpStackSize=65536")
//@ runDefault("--useRegExpJIT=0", "--maxRegExpStackSize=65536")
//@ runDefault

// The flat program keeps one frame of 16 bytes for each iteration of a greedy group and pushes one more when it tries the
// iteration that fails, so with 65536 bytes /x(?:ab)*/ gives up from 4096 iterations on. The bytecode interpreter stores nothing
// for an iteration of a group that ends the pattern. Its first pool is one page, and no page is larger than 65536 bytes.
const maxFrames = 65536 / 16;

function shouldBe(actual, expected, label) {
    if (actual !== expected)
        throw new Error(label + ": " + actual + " instead of " + expected);
}

const groups = [
    ["x(?:ab)*", "ab"],
    ["x(?:ab)+", "ab"],
    ["x(?:ab|cd)*", "cd"],
    ["x(?:[ab]c)*", "bc"],
    ["x(?:Жb)*", "Жb"],
];
const iterationCounts = [1, maxFrames - 2, maxFrames - 1, maxFrames, maxFrames + 1, 3 * maxFrames];
const prefixes = ["", "yy", "Ж"];
const suffixes = ["", "a", "x"];

for (const [source, iteration] of groups) {
    const regexp = new RegExp(source);
    for (const iterations of iterationCounts) {
        const matched = "x" + iteration.repeat(iterations);
        for (const prefix of prefixes) {
            for (const suffix of suffixes) {
                const label = "/" + source + "/ with " + iterations + " iterations after " + JSON.stringify(prefix) + " and before " + JSON.stringify(suffix);
                const match = regexp.exec(prefix + matched + suffix);
                shouldBe(match.index, prefix.length, label + ", index");
                shouldBe(match[0].length, matched.length, label + ", length");
                shouldBe(match[0] === matched, true, label + ", text");
            }
        }
        shouldBe(regexp.test(iteration.repeat(iterations)), false, "/" + source + "/ without x");
    }
}
