//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")
//@ runDefault("--useRegExpImageMode=1")
//@ runDefault

// Where a pattern has references, what is left of a search depends on what the referenced groups hold as well as on the position. The table of failed searches has an entry for each set of their bounds,
// so a search that comes to one place again and again with a few different captures is made once for each. The validation runs the flat programs beside the bytecode on every match, with the table from
// the start, and stops where the results or the turns differ.
function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}

const bodies = [
    "(a+)+\\1b", "(?<n>a+)+\\k<n>b", "(a|aa)+\\1b", "(a+)+?\\1b", "(a+)(a*)+\\1\\2b", "(?:(a+)|(aa))+\\1\\2b", "((a+)+)\\2b", "(a+)+(?:\\1)*b", "(a)(?:a|a)*\\1b", "(?:(a)|a)*\\1b", "(a+)+\\1{2}b",
    "(a+)+(?=\\1)ab", "(a+)+(?!\\1b)a", "(a+)(b+)(c*)(d*)(?:a|a)*\\1\\2\\3\\4x", "(a+)(b+)(c*)(d*)(e*)(?:a|a)*\\1\\2\\3\\4\\5x", "(\\w+)\\s(?:\\w+\\s*)+\\1:", "(a*)+\\1b", "(?:(a+)\\1)+b", "(?:(a)|(b))+\\2\\1c",
];
const around = ["X", "X|$", "(?:^X)|$", "(?:X)|zz", "(?:^X|never)|$", "^X$"];
const subjects = [
    "", "a", "aaaaaaaaaaac", "aaaaaaaaaaab", "aaaaaaaaaaaczz", "caaaaaaaab", "abaaaaaaaaaby", "abaaaaaaaaabx", "ab ab ab ab ;", "ab ab ab ab:", "ab cd ab ab:", "aabbaabbc", "ababababc", "AaAaAaAab",
];
let matches = 0;
for (const flags of ["", "i", "g", "u"]) {
    for (const body of bodies) {
        for (const form of around) {
            const pattern = new RegExp(form.replaceAll("X", body), flags);
            for (const subject of subjects) {
                for (const text of [subject, subject + "ā"]) {
                    for (const lastIndex of [0, 1]) {
                        pattern.lastIndex = lastIndex;
                        if (pattern.exec(text))
                            ++matches;
                    }
                }
            }
        }
    }
}
shouldBe(matches > 2000, true, "matches");
shouldBe(new RegExp("(?:^(a+)+\\1b)|$").exec("a".repeat(16) + "c").index, 17, "the alternative behind the group");
shouldBe(new RegExp("^(a+)+\\1b").exec("a".repeat(16) + "b")[1], "a", "what the group holds");
