//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")
//@ runDefault

// A pattern without the flag m that has a ^ in one alternative and none in another is taken apart: all its alternatives are tried at the first place, and behind them, there and at every later place,
// copies that lack what begins with ^. A flat program that counts turns walks them in that order. The validation runs it beside the bytecode on every match, and stops where the two have not counted
// the same number of turns. It also compares the answer of the flat program, which leaves places out by a text that every match holds and gives no character of some runs back, with the bytecode's.
function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}

const bodies = [
    "(a+)+b", "(?:a+)+b", "(a|aa)+b", "(?:a|a)*b", "(a+?)+?b", "(a+){2,}b", "(?:a+){1,4}b", "((a+)+)+b", "(?:a|b|ab)*c", "(?:\\w+\\s*)+:", "(?:.*,)*x", "(a+)+\\1b", "(?=(a+)+b)a", "(?!(a+)+b)a",
    "q(?:/[^/]+)*/[^/]*a[^/]*a[^/]*(?:/[^/]+)*/b", "q/[^/]*a[^/]*(?:/[^/]+)*/b", "k\\s+(?:\\w+=\\S*\\s+)*end", "(?:a+b?)*c", "(?:a|a){3,}b", "(?:(a)|(b)|(ab))*c", "(?:/([^/]+))*/x",
];
const around = [
    "^X|$", "(?:^X)|$", "^(?:X)|$", "(?:^X)|zz", "^X|^never|$", "(?:^X|^never)|$", "(?:^X|never)|$", "(?:^X$)|$", "(?:^|,)X|zz", "(?:(?:^X)|zz)", "((?:^X)|(zz))", "$|^X|zz", "^X|X|zz", "(?:^X|X)z|zz",
    "(?:^a|b)X|zz", "^(?:X)+|zz", "(?:^X|(?:a|a)*z)|zz", "X|$", "(?:X)|zz", "X",
];
const subjects = [
    "", "a", "aaaaaaaaac", "aaaaaaaaab", "aaaaaaaaaczz", "caaaaaaab", "aaaaaacab", "ababababd", "ab ab ab ab ;", "ab ab ab:", "a,a,a,a,c", "a,a,x", "q/aaa/m/aaaaaa/c", "q/aaa/m/aaaaaa/b", "q/aaaaaaaa/c",
    "k A=k A=k A=k\nno", "k A=k A=k end", ",aaaaaab", "zz", "aaaaaaaaaz", "/a/bb/ccc/x", "/a/bb/ccc/y",
];
let matches = 0;
for (const flags of ["", "i", "g", "y", "u"]) {
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
shouldBe(matches > 5000, true, "matches");
shouldBe(new RegExp("(?:^(a+)+b)|$").exec("aaaaaac").index, 7, "the alternative behind the group");
shouldBe(new RegExp("(?:^(a+)+b)|a").exec("aaaaaac").index, 0, "the copy at the first place");
shouldBe(new RegExp("(a+)+b").exec("aaaaaacab").index, 7, "the match behind the places that hold no match");
