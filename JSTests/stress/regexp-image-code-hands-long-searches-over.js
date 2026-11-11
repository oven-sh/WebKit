//@ runDefault("--useRegExpImageMode=1")
//@ runDefault

// The code of a pattern in an image stops after a number of turns of its groups that an ordinary search does not come near. The match is then made again as for a pattern made at run time:
// with a table of the searches that have failed, which finds the match, or gives it up at the turn at which the JIT gives it up, in a time in proportion to the length of the subject.
function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + " instead of " + expected);
}

function indexOf(pattern, subject) {
    const match = pattern.exec(subject);
    return match ? match.index : null;
}

// The turns double with each character: a hundred million are reached at 27.
for (const pattern of [/(?:^(x+)+y)|$/, /(?:(x+)+y)|zz/, /^(?:x+)+y|^never|$/]) {
    shouldBe(indexOf(pattern, "x".repeat(12) + "czz") !== null, true, pattern + " at 12");
    shouldBe(indexOf(pattern, "x".repeat(22) + "czz") !== null, true, pattern + " at 22");
    shouldBe(indexOf(pattern, "x".repeat(30) + "czz"), null, pattern + " at 30");
}
// The turns grow with the fourth power of the length: a hundred million are reached at 224.
const path = /^q(?:\/[^\/]+)*\/[^\/]*x[^\/]*x[^\/]*x[^\/]*(?:\/[^\/]+)*\/[^\/]*x[^\/]*\/y(?=$|\/$)/;
const pathOrEnd = /(?:^q(?:\/[^\/]+)*\/[^\/]*x[^\/]*x[^\/]*x[^\/]*(?:\/[^\/]+)*\/[^\/]*x[^\/]*\/y(?=$|\/$))|$/;
for (const [length, index] of [[20, 0], [223, 0], [224, null], [900, null]]) {
    shouldBe(indexOf(path, "q/xxx/m/" + "x".repeat(length) + "/y"), index, "the path at " + length);
    shouldBe(indexOf(pathOrEnd, "q/xxx/m/" + "x".repeat(length) + "/y"), index, "the path or the end at " + length);
}
// A search that is long because its subject is long goes on.
shouldBe(indexOf(/(?:[a-z]+\.)+com/, "lorem ipsum dolor sit amet ".repeat(40000) + "www.example.com"), 27 * 40000, "a long subject");
// With lastIndex, and with captures.
const sticky = /(?:(x+)+y)|(x)/y;
sticky.lastIndex = 2;
shouldBe(sticky.exec("zz" + "x".repeat(20) + "c")[2], "x", "the second alternative at lastIndex");
