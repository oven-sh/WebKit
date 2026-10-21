//@ defaultRun
//@ runDefault("--useRegExpJIT=0")
//@ runDefault("--useLazyRegExpPatternConstruction=0")

// A path of a pattern that needs more input than a string can hold (String::MaxLength, 2^31 - 1 code units)
// can never match. It fails like any other path. The pattern is not a SyntaxError, at creation or at a match.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": expected " + expected + " but got " + actual);
}

// null, or the index, the match and the captures.
function show(values) {
    if (values === null)
        return "null";
    return values.map(value => value === undefined ? "undefined" : JSON.stringify(value)).join(",");
}

function exec(regExp, string) {
    const match = regExp.exec(string);
    return show(match && [match.index, ...match]);
}

const filler = "x".repeat(80);
const letters = "bcdefghijklmnopqr".split("");
// Nine alternatives with the same first character. The pattern build merges them under it.
const merged = letters.slice(0, 9).map(letter => "a" + letter).join("|");

// [source, flags, subject, expected]. expected is null, or the index, the match and the captures.
const rows = [
    // The byte compiler's sums pass UINT_MAX: the error came at the first match.
    ["(?:z)?x{4294967295}", "", "", null],
    ["(?:z)?x{4294967295}", "", "x", null],
    ["(?:z)?x{4294967295}", "", "zx", null],
    ["(?:zy)*a{4294967294,}", "", "", null],
    ["(?:zy)*a{4294967294,}", "", "zya", null],
    ["(?:z)?x{4294967295}|q", "", "q", [0, "q"]],
    ["(?:z)?x{4294967295}|q", "", "x", null],
    ["(?<=a{4294967295})b", "", "b", null],
    ["(?<=a{4294967295})b", "", "ab", null],
    ["(?<!a{4294967295})b", "", "b", [0, "b"]],
    ["(?<!a{4294967295})b", "", "ab", [1, "b"]],
    ["(?:q)?b{9007199254740991}", "", "", null],
    ["(?:q)?b{9007199254740991}", "", "qb", null],
    [filler + "(?:a|b{4294967215})c", "", filler + "ac", [0, filler + "ac"]],
    [filler + "(?:a|b{4294967215})c", "", filler + "bc", null],
    ["(?:(?:a{2147483647})*b{2147483647})*c{2147483647}", "", "", null],
    ["(?:(?:a{2147483647})*b{2147483647})*c{2147483647}", "", "c", null],
    ["(?:(?:a{2147483647})+b{2147483647})+c{2147483647}", "", "", null],
    ["(?:(?:a{2147483647})+b{2147483647})+c{2147483647}", "", "abc", null],
    ["(?:(?:(a{2147483647}))?b{2147483647})?c{2147483647}\\1", "", "", null],
    ["(?:(?:(a{2147483647}))?b{2147483647})?c{2147483647}\\1", "", "c", null],
    ["(?:a{2147483648}|)a{2147483648}|bc", "", "x", null],
    ["(?:a{2147483648}|)a{2147483648}|bc", "", "bc", [0, "bc"]],
    ["(?<!(?<=a{4294967295})b|c)d", "", "d", [0, "d"]],
    ["(?<!(?<=a{4294967295})b|c)d", "", "cd", null],
    ["(?<!(?<=a{4294967295})b|c)d", "", "bd", [1, "d"]],
    ["b(?<!a{4294967294}c)|(?:z)?x{4294967295}", "", "b", [0, "b"]],
    ["b(?<!a{4294967294}c)|(?:z)?x{4294967295}", "", "acb", [2, "b"]],
    ["b(?<!a{4294967294}c)|(?:z)?x{4294967295}", "", "zx", null],
    [merged + "|a(?:z)?x{4294967294}", "", "aj", [0, "aj"]],
    [merged + "|a(?:z)?x{4294967294}", "", "azx", null],
];

// Over 64 characters a RegExp is created without its YarrPattern (--useLazyRegExpPatternConstruction).
const longPrefix = "(?:" + "y".repeat(70) + ")?";

for (const [source, flags, subject, expected] of rows) {
    for (const text of [source, longPrefix + source]) {
        const what = "/" + text + "/" + flags + " on " + JSON.stringify(subject);
        const regExp = new RegExp(text, flags);
        shouldBe(exec(regExp, subject), show(expected), what);
        shouldBe(regExp.test(subject), expected !== null, what + ", test()");
        // A RegExp with the same text, created after a match, and used for the other call first.
        const again = new RegExp(text, flags);
        shouldBe(again.test(subject), expected !== null, what + ", second RegExp, test()");
        shouldBe(exec(again, subject), show(expected), what + ", second RegExp");
    }
}

// One RegExp, first in JIT code and then in the interpreter. No alternative of these patterns needs more than
// String::MaxLength, so they get JIT code, which answers "ac". On "c" an iteration of the first group matches
// nothing: the JIT code gives the match to the interpreter, and the sums of the byte compiler pass UINT_MAX for
// the lookbehind. The same object then answered "ac" with a SyntaxError.
{
    const head = "(?:(a)|b*)+?c";
    const max = "2147483647";
    // [lookbehind, flags, expected for "ac", "c", "bc", "\u0100ac", "\u0100c"]
    const tails = [
        [`(?<!(?<=(?<=x{${max}})y{${max}})z{${max}})`, "", '0,"ac","a"', '0,"c",undefined', '0,"bc",undefined', '1,"ac","a"', '1,"c",undefined'],
        [`(?<=(?<=(?<=x{${max}})y{${max}})z{${max}}|ac)`, "", '0,"ac","a"', "null", "null", '1,"ac","a"', "null"],
        [`(?<=(?<=(?<!x{${max}})y{${max}})z{${max}}|(a)c)`, "", '0,"ac","a","a"', "null", "null", '1,"ac","a","a"', "null"],
        [`(?<!(?<=(?<!x{${max}})y{${max}})z{${max}}|bc)`, "", '0,"ac","a"', '0,"c",undefined', "null", '1,"ac","a"', '1,"c",undefined'],
        [`(?<=(?<=(?<=q|x{${max}})y{${max}})z{${max}}|ac)`, "", '0,"ac","a"', "null", "null", '1,"ac","a"', "null"],
        [`(?<!(?<=(?<=q|x{${max}})y{${max}})z{${max}}|bc)`, "i", '0,"ac","a"', '0,"c",undefined', "null", '1,"ac","a"', '1,"c",undefined'],
    ];
    for (const [tail, flags, ac, c, bc, wideAc, wideC] of tails) {
        const regExp = new RegExp(head + tail, flags);
        const what = "/" + regExp.source + "/" + flags;
        shouldBe(regExp.test("ac"), true, what + ", test(), JIT code");
        shouldBe(exec(regExp, "ac"), ac, what + ", JIT code");
        shouldBe(exec(regExp, "c"), c, what + ", interpreter");
        shouldBe(exec(regExp, "ac"), ac, what + ", after the interpreter ran");
        shouldBe(regExp.test("ac"), true, what + ", test(), after the interpreter ran");
        shouldBe(exec(regExp, "bc"), bc, what + ", interpreter again");
        shouldBe(exec(regExp, "\u0100ac"), wideAc, what + ", 16-bit");
        shouldBe(exec(regExp, "\u0100c"), wideC, what + ", 16-bit, interpreter");
        shouldBe(exec(new RegExp(regExp.source, flags), "ac"), ac, what + ", second RegExp");
    }
}

// An 8-bit subject, then a 16-bit one, and the other order.
{
    const first = new RegExp("(?:z)?x{4294967295}|q");
    shouldBe(first.test("q"), true, "8-bit");
    shouldBe(first.test("\u0100q"), true, "16-bit after 8-bit");
    shouldBe(first.test("\u0100x"), false, "16-bit after 8-bit, no match");
    const second = new RegExp("(?<!a{4294967295})b|(?:w)?x{4294967295}");
    shouldBe(exec(second, "\u0100ab"), '2,"b"', "16-bit");
    shouldBe(exec(second, "ab"), '1,"b"', "8-bit after 16-bit");
}

// Sticky, from a lastIndex past the start.
{
    const sums = new RegExp("(?:z)?x{4294967295}|(?<!a{4294967295})c", "y");
    sums.lastIndex = 2;
    shouldBe(exec(sums, "zxc"), '2,"c"', "sticky from 2");
    shouldBe(sums.lastIndex, 3, "sticky, lastIndex after a match");
    sums.lastIndex = 1;
    shouldBe(sums.test("zxc"), false, "sticky from 1");
    shouldBe(sums.lastIndex, 0, "sticky, lastIndex after no match");
}

shouldBe("abc".replace(/(?:z)?x{4294967295}/, "y"), "abc", "replace");
shouldBe("abc".split(/(?:z)?x{4294967295}/).length, 1, "split");
shouldBe("abc".search(/(?:z)?x{4294967295}/), -1, "search");
shouldBe("abc".search(/(?<!a{4294967295})c/), 2, "search with a lookbehind");

function errorOf(run) {
    try {
        run();
    } catch (error) {
        return String(error);
    }
    return "no error";
}

function testNoInline(regExp, string) {
    return regExp.test(string);
}
noInline(testNoInline);

// What a first match still refuses, and every match after it: an error that only the pattern build reports, for
// a pattern over 64 characters. It is that error at creation or at each use, never a result.
{
    const refused = "SyntaxError: Invalid regular expression: negated class set may contain strings";
    let regExp = null;
    const atCreation = errorOf(() => { regExp = new RegExp("abc[^\\q{}](?:" + "y".repeat(70) + ")?", "v"); });
    if (!regExp)
        shouldBe(atCreation, refused, "refused at creation");
    else {
        for (let i = 0; i < 100; ++i)
            shouldBe(errorOf(() => testNoInline(regExp, "x")), refused, "refused, test()");
        shouldBe(errorOf(() => regExp.exec("abcd")), refused, "refused, exec()");
        shouldBe(errorOf(() => "x".replace(regExp, "y")), refused, "refused, replace");
    }
}

function testLiteral(string) {
    return /(?:z)?x{4294967295}|q/.test(string);
}
noInline(testLiteral);

function execLiteral(string) {
    return /(?<!a{4294967295})b|(?:w)?(x{4294967295})/.exec(string);
}
noInline(execLiteral);

function testCreated(source, string) {
    return new RegExp(source).test(string);
}
noInline(testCreated);

const createdSources = [];
const createdSubjects = [];
for (let i = 0; i < 8; ++i) {
    createdSources.push("(?:c" + i + ")?a{4294967295}|d" + i);
    createdSubjects.push(i & 1 ? "d" + i : "c" + i + "a");
}

for (let i = 0; i < testLoopCount; ++i) {
    shouldBe(testLiteral("zx"), false, "literal, no match");
    shouldBe(testLiteral("aq"), true, "literal");
    const match = execLiteral("ab");
    shouldBe(match.index, 1, "literal, exec(), index");
    shouldBe(match[1], undefined, "literal, exec(), capture");
    shouldBe(execLiteral("wx"), null, "literal, exec(), no match");
    shouldBe(testCreated(createdSources[i & 7], createdSubjects[i & 7]), !!(i & 1), "created in a loop");
}

// Last in the file: compile() makes the DFG and the FTL stop treating a RegExp of this realm as a constant.
{
    const regExp = /abc/;
    regExp.compile("(?:z)?x{4294967295}|q");
    shouldBe(regExp.source, "(?:z)?x{4294967295}|q", "compile(), source");
    shouldBe(regExp.test("q"), true, "compile(), test()");
}
