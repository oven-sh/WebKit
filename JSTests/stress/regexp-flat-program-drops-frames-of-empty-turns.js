//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1", "--maxRegExpStackSize=65536")
//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")
//@ runDefault

// A loop at the end of a pattern whose turns may take nothing keeps frames for each of its turns, and gives up where it may not have more (here: 4096). The match is made again on a flat program that drops
// the frames of the turns it has made. A turn that took nothing is made otherwise, as it is in the program that keeps them.
function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}

// Counts of turns that keep their frames are asked with each prefix and suffix, and the long ones with one of them.
const shortTurns = [1, 2];
const longTurns = [1365, 4097];

// Each takes all of "abab..".
const bodies = ["a*b*", "a?b?", "(?:ab)?", "|a|b", "a||b", "a*|b", "a|b*", "b*a*", "a?(?:b|)", "(?:a|)(?:|b)", "(?:a*)*b?", "a*?b?", "(?=a)a?|b", "(?!b)a?|b", "\\b|a|b", "(?:a|b?){0,3}", "[ab]?", "a?b?(?=.|$)"];
for (const body of bodies) {
    for (const [source, flags] of [["x(?:" + body + ")*", ""], ["x(?:#|(?:" + body + ")*)", "s"], ["x(?:" + body + ")*", "i"], ["x(?:" + body + ")*", "u"], ["x(?:" + body + ")+", "g"]]) {
        const expression = new RegExp(source, flags);
        function check(count, prefix, suffix) {
            const what = expression + " " + count + " " + JSON.stringify([prefix, suffix]);
            expression.lastIndex = 0;
            const match = expression.exec(prefix + "x" + "ab".repeat(count) + suffix);
            shouldBe(match === null, false, what);
            shouldBe(match.index, prefix.length, what);
            shouldBe(match[0].length, 1 + 2 * count, what);
        }
        for (const prefix of ["", "yy", "Ж"]) {
            for (const suffix of ["", "z", "Ж"]) {
                for (const count of shortTurns)
                    check(count, prefix, suffix);
            }
        }
        for (const count of longTurns)
            check(count, "yy", "z");
        expression.lastIndex = 0;
        shouldBe(expression.test("y" + "ab".repeat(4097)), false, expression + " without x");
    }
}

// A loop that is lazy takes nothing, and a loop with a bound no more than the bound.
shouldBe(/x(?:a?b?)*?/.exec("x" + "ab".repeat(12000))[0].length, 1, "lazy");
shouldBe(new RegExp("x(?:a?b?){0,1000}").exec("x" + "ab".repeat(12000))[0].length, 1 + 2 * 1000, "bound");
// A capture in the loop can be asked for, so its turns keep what they captured.
const captures = new RegExp("x(?:(a)?(b)?)*").exec("x" + "ab".repeat(100));
shouldBe(captures[0].length + " " + captures[1] + " " + captures[2], "201 a b", "captures");
