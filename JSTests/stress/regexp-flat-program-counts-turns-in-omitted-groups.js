//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")
//@ runDefault

// The fast flat program omits an optional group that consumes nothing. The JIT runs that group and counts the turns of the repeated groups inside it, so the bytecode behind the flat program has to try
// every start, as it does for any pattern whose turns the JIT counts. The validation stops where the flat program that counts turns and the bytecode have not counted the same number.
function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}

const repeated = ["(b)*", "(b){1,}", "(?:)*?", "(?:b|c)+", "(?:b)*?c", "(b+)+", "a*?(b){1,}(?<=^|\\s)", "[^c]+|", "(?:b|b){2,4}"];
const omitted = ["(?:(?=X))?", "(?:(?!X))?", "(?:(?=X))*", "(?:(?=X)){0,3}", "(?:(?=X))??", "(?:(?<=X))?", "(?:(?=X)(?:)*?)?", "(?:(?:(?=X))?)?", "(?:\\b(?=X))?", "((?=X))?"];
const rest = ["c$", "[bc][bc]$", "(?<=[ab]c)$", "[a-c]*\\(b??", "bc", "(?:b|c)*d"];
const subjects = ["", "b", "bbbbc", "bbbbbbbbbbbbc", "\n=\u{1f600}=ééc", "\nab ", "abcabc(", "cbcbcbd", " bc bc"];
let matches = 0;
for (const flags of ["", "i", "m", "s", "u", "v", "g", "y"]) {
    for (const inner of repeated) {
        for (const group of omitted) {
            for (const tail of rest) {
                const pattern = new RegExp(group.replaceAll("X", inner) + tail, flags);
                for (const subject of subjects) {
                    pattern.lastIndex = 0;
                    if (pattern.exec(subject))
                        ++matches;
                }
            }
        }
    }
}
shouldBe(matches > 5000, true, "matches");

const found = new RegExp("(?:(?=(b)*))?[bc][bc]$").exec("bbbbc");
shouldBe(found.index, 3, "the match behind the omitted group");
shouldBe(found[1], undefined, "the capture of the omitted group");
