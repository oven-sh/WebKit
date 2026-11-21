//@ runDefault
//@ runDefault("--useRegExpImageMode=1")
//@ runDefault("--useRegExpJIT=0")
//@ runDefault("--useRegExpJIT=0", "--validateRegExpFlatProgram=1")
//@ runDefault("--useConcurrentJIT=0", "--maximumRegExpTestInlineCodesize=10000")

// A \k<name> inside a group of that name matches the empty string: that group has not
// captured yet where the reference runs, and every other group of the name is in another
// alternative. This holds when several groups share the name, and the reference is not
// in the first of them. The expected values are those of V8, and this file runs in node
// as it is.

var failures = [];

function describe(match)
{
    if (!match)
        return "null";
    var result = "[";
    for (var i = 0; i < match.length; ++i)
        result += (i ? "," : "") + (match[i] === undefined ? "undefined" : JSON.stringify(match[i]));
    return result + "]@" + match.index;
}

function shouldBe(actual, expected, what)
{
    if (actual !== expected)
        failures.push(what + ": expected " + expected + " but got " + actual);
}

function test(regExp, subject, expected)
{
    var what = regExp + " on " + JSON.stringify(subject);

    // test() runs the code that records no captures.
    shouldBe(regExp.test(subject), expected !== "null", "test " + what);

    regExp.lastIndex = 0;
    shouldBe(describe(regExp.exec(subject)), expected, "exec " + what);
}

// A turn of the repeated group has to match the empty string, then the I takes the next character.
test(/(?:(?<x>z)|(?<x>(?:I?)\k<x>)){2,}I/, "aIIcx", '["II",undefined,""]@1');
test(/(?:(?<x>z)|(?<x>I?\k<x>)){2,}I/, "cIIcIIx", '["II",undefined,""]@1');
test(/(?:(?<x>z)|(?<x>(?:I?)\k<x>{1,1})){2,}I/, "aIIcx", '["II",undefined,""]@1');
test(/(?:(?<x>qcIIx)|(?<x>I?\k<x>+)){2,}I/, "cIIcIIx", '["II",undefined,""]@1');
test(/(?:(?<x>z)|(?<x>I?\k<x>))I/, "Ix", '["I",undefined,""]@0');

// The group closes once, the b+ behind it fails, and matching goes back into the group.
test(/(?:(?<x>a+)|(?<x>b*\k<x>))b+/, "bc", '["b",undefined,""]@0');
test(/(?:(?<x>a+)|(?<x>b*\k<x>))b+/, "bbcb", '["bb",undefined,"b"]@0');
test(/(?<x>a+)|(?<x>b*\k<x>)b+/, "bc", '["b",undefined,""]@0');
test(/c(?:(?<x>a+)|(?<x>b*\k<x>))b+/, "cbbc", '["cbb",undefined,"b"]@0');
test(/b(?:(?<x>cc)|(?<x>(?:a*){2}\k<x>))a/, "bab", '["ba",undefined,""]@0');
test(/(?:(?<x>b)|(?<x>ab+\k<x>{2}b)){2,3}/, "aabbb", '["abbb","b",undefined]@1');

// Three groups of the name. The reference is in the middle one, then in the last one.
test(/(?:(?<x>a+)|(?<x>b*\k<x>)|(?<x>c))b+/, "bc", '["b",undefined,"",undefined]@0');
test(/(?:(?<x>c)|(?<x>a+)|(?<x>b*\k<x>))b+/, "bc", '["b",undefined,undefined,""]@0');

// The reference is deeper in the group: in a group, in a capture, in a lookahead.
test(/(?:(?<x>a+)|(?<x>b*(?:c|\k<x>)))b+/, "bc", '["b",undefined,""]@0');
test(/(?:(?<x>a+)|(?<x>b*(\k<x>)))b+/, "bc", '["b",undefined,"",""]@0');
test(/(?:(?<x>a+)|(?<x>b*(?<y>\k<x>)))b+/, "bc", '["b",undefined,"",""]@0');
test(/(?:(?<x>z)|(?<x>a*(?=\k<x>a)))(?=aab)/, "aaaab", '["aa",undefined,"aa"]@0');
test(/(?:(?<x>z)|(?<x>a*(?!\k<x>b)))(?=aab)/, "aaab", '["a",undefined,"a"]@0');

// A quantifier on the reference.
test(/(?:(?<x>a+)|(?<x>b*\k<x>+))b+/, "bc", '["b",undefined,""]@0');
test(/(?:(?<x>a+)|(?<x>b*\k<x>+?))b+/, "bc", '["b",undefined,""]@0');
test(/(?:(?<x>a+)|(?<x>b*\k<x>{2}))b+/, "bc", '["b",undefined,""]@0');
test(/(?:(?<x>a+)|(?<x>b*\k<x>{2,}))b+/, "bc", '["b",undefined,""]@0');
test(/(?:(?<x>a+)|(?<x>b*\k<x>*))b+/, "bc", '["b",undefined,""]@0');
test(/(?:(?<x>a{1,2})|(?<x>a*\k<x>?))+?b/, "aaac", "null");

// A quantifier on the group that holds the reference, and on the group around it.
test(/(?:(?<x>a+)|(?<x>b*\k<x>)+)b+/, "bc", '["b",undefined,""]@0');
test(/(?:(?<x>a+)|(?<x>b*\k<x>){2})b+/, "bbc", '["bb",undefined,""]@0');
test(/(?:(?<x>a+)|(?<x>b*\k<x>))+b+/, "bbc", '["bb",undefined,"b"]@0');
test(/(?:(?<x>a+)|(?<x>b*\k<x>)){2}b+/, "bbc", '["bb",undefined,""]@0');

// A group with a fixed count, where the reference was the only term that matching could go back into.
test(/(?:(?<x>a)|(?<x>b\k<x>){2})c/, "bbc", '["bbc",undefined,"b"]@0');
test(/(?:(?<x>a)|(?<x>b\k<x>){2})c/, "bc", "null");
test(/(?:(?<x>a)|(?:(?<x>b\k<x>)){3})c/, "abbbc", '["bbbc",undefined,"b"]@1');

// A lookbehind in the group holds the reference.
test(/(?:(?<x>z)|(?<x>a*(?<=\k<x>a)))(?=ab)/, "aaab", '["aa",undefined,"aa"]@0');
test(/(?<x>z)|(?<x>[ab]*?(?<=\k<x>a))$/, "aaba", '["aaba",undefined,"aaba"]@0');

// A lookbehind holds the groups and the reference. Its terms run from right to left.
test(/(?<=b(?:(?<x>a)|(?<x>\k<x>b*)))c/, "bc", '["c",undefined,""]@1');
test(/(?<=b(?:(?<x>a)|(?<x>\k<x>b*)))c/, "cbc", '["c",undefined,""]@2');
test(/(?<=b(?:(?<x>a)|(?<x>\k<x>b*)))c/, "bbbc", '["c",undefined,"bb"]@3');
test(/(?<!b(?:(?<x>a)|(?<x>\k<x>b*)))c/, "bc", "null");
test(/(?<=^(?:(?<x>z)|(?<x>\k<x>a*?)))b/, "aab", '["b",undefined,"aa"]@2');
test(/(?<=(?:(?<x>ba)|(?<x>\k<x>+b+)){2,})b/, "aabbb", '["b",undefined,"b"]@4');

// A {0} takes the reference out. The reference to y in front of it stays a backreference.
test(/(?<=\k<y>(?:(?<x>z)|(?<x>\k<x>{0}a))(?<y>c))b/, "cacb", '["b",undefined,"a","c"]@3');
test(/(?<=\k<y>(?:(?<x>z)|(?<x>\k<x>{0}a))(?<y>c))b/, "xacb", "null");

// A lookahead in a lookbehind, and a lookbehind in a lookahead in a lookbehind.
test(/(?<=(?=(?:(?<x>z)|(?<x>a*(?<=\k<x>a)))(?=ab))a*)b/, "aaab", '["b",undefined,"aa"]@3');
test(/(?<=a(?:(?<x>z)|(?<x>b(?=(?<=\k<x>b))b*?)))c/, "abbc", '["c",undefined,"bb"]@3');

// Flags, a subject that is not 8-bit, a pattern that is not 8-bit.
test(/(?:(?<x>A+)|(?<x>b*\k<x>))b+/i, "Bc", '["B",undefined,""]@0');
test(/(?:(?<x>a+)|(?<x>b*\k<x>))b+/u, "bc", '["b",undefined,""]@0');
test(/(?:(?<x>a+)|(?<x>b*\k<x>))b+/v, "bc", '["b",undefined,""]@0');
test(/(?:(?<x>a+)|(?<x>b*\k<x>))b+/y, "bc", '["b",undefined,""]@0');
test(/(?:(?<x>a+)|(?<x>b*\k<x>))b+/, "\u0100bc", '["b",undefined,""]@1');
test(/(?:(?<x>\u0100+)|(?<x>\u0101*\k<x>))\u0101+/, "\u0101\u0100", '["\u0101",undefined,""]@0');
test(/(?:(?<x>a+)|(?<x>[\u{1F600}b]*\k<x>))[\u{1F600}b]+/u, "\u{1F600}c", '["\u{1F600}",undefined,""]@0');

// An illegal \9 makes the parser read the pattern a second time.
test(/(?<x>z)|(?<x>a*?\k<x>)$|\9/, "aa", '["aa",undefined,"aa"]@0');

// These references are not in a group of their name. They read the capture.
test(/(?:(?<x>a+)|(?<x>b*\k<x>))c\k<x>/, "bbcbb", '["bbcbb",undefined,"bb"]@0');
test(/(?:(?<x>a+)|(?<x>b*\k<x>))c\k<x>/, "bbcb", '["bcb",undefined,"b"]@1');
test(/(?:(?<x>a)|(?<x>b))\k<x>/, "abb", '["bb",undefined,"b"]@1');
test(/(?:(?<x>a)|(?<x>b))\k<x>/, "ab", "null");
test(/(?<=\k<x>(?:(?<x>z)|(?<x>a)))b/, "aab", '["b",undefined,"a"]@2');
test(/(?<=\k<x>(?:(?<x>z)|(?<x>a)))b/, "cab", "null");
test(/(?<=(?:(?<x>z)|(?<x>\k<x>a))\k<x>)b/, "aab", '["b",undefined,"a"]@2');

// The same with a quantifier on the reference.
test(/(?:(?<x>a)|(?<x>b))\k<x>*bc/, "bbbc", '["bbbc",undefined,"b"]@0');
test(/(?:(?<x>a)|(?<x>b))\k<x>+?c/, "bbbc", '["bbbc",undefined,"b"]@0');
test(/(?:(?<x>a)|(?<x>b))\k<x>{2,}c/, "bbbc", '["bbbc",undefined,"b"]@0');
test(/(?:(?<x>a)|(?<x>b))\k<x>{2,}c/, "bbc", "null");
test(/(?:(?<x>a)|(?<x>b))?\k<x>*c/, "c", '["c",undefined,undefined]@0');
test(/(?:(?<x>a)|(?<x>b))?\k<x>*?c/, "d", "null");
test(/(?:(?<x>A)|(?<x>B))\k<x>+?c/i, "bBbc", '["bBbc",undefined,"b"]@0');
test(/(?:(?<x>\u{1F600})|(?<x>\u{1F601}))\k<x>*c/u, "\u{1F601}\u{1F601}\u{1F601}c", '["\u{1F601}\u{1F601}\u{1F601}c",undefined,"\u{1F601}"]@0');
test(/(?:(?<x>a)|(?<x>b))\k<x>*bc/, "\u0100bbbc", '["bbbc",undefined,"b"]@1');

// The reference is in the first group of the name.
test(/(?:(?<x>b*\k<x>)|(?<x>a+))b+/, "bc", '["b","",undefined]@0');

// One object: test, exec, test. Then an 8-bit, a 16-bit and an 8-bit subject.
var regExp = /(?:(?<x>a+)|(?<x>b*\k<x>))b+/;
shouldBe(regExp.test("bc"), true, "test before exec");
shouldBe(describe(regExp.exec("bc")), '["b",undefined,""]@0', "exec between two tests");
shouldBe(regExp.test("bc"), true, "test after exec");
shouldBe(describe(regExp.exec("\u0100bc")), '["b",undefined,""]@1', "16-bit subject after an 8-bit one");
shouldBe(describe(regExp.exec("bc")), '["b",undefined,""]@0', "8-bit subject after a 16-bit one");

// The other ways to run a pattern read the same captures.
var global = /(?<x>z)|(?<x>a*?\k<x>)$/g;
shouldBe(JSON.stringify("aa".match(global)), '["aa",""]', "match");
shouldBe("aa".replace(global, "[$<x>]"), "[aa][]", "replace");
shouldBe("aa".search(global), 0, "search");
shouldBe(JSON.stringify("caab".split(/(?:(?<x>z)|(?<x>a*?\k<x>))b/)), '["c",null,"aa",""]', "split");
shouldBe(JSON.stringify(Array.from("aa".matchAll(global), match => match.index)), "[0,2]", "matchAll");
shouldBe(/(?<x>z)|(?<x>a*?\k<x>)$/.exec("aa").groups.x, "aa", "groups");

// Many calls, so that the optimizing tiers compile the calls and inline test(). One subject matches and one does not.
// The last run line of this file makes those compiles happen at a fixed number of calls.
function hotTest(subject)
{
    return /(?:(?<x>a+)|(?<x>b*\k<x>))b+/.test(subject);
}

function hotExec(subject)
{
    return /(?:(?<x>a+)|(?<x>b*\k<x>))b+/.exec(subject);
}

if (typeof noInline === "function") {
    noInline(hotTest);
    noInline(hotExec);
}

var loopCount = typeof testLoopCount === "number" ? testLoopCount : 10000;
for (var i = 0; i < loopCount; ++i) {
    var tested = hotTest("bc");
    var match = hotExec("bc");
    if (!tested || !match || match[0] !== "b" || match[2] !== "") {
        failures.push("call " + i + " of the hot functions: test() returned " + tested + " and exec() returned " + describe(match));
        break;
    }
    tested = hotTest("cc");
    match = hotExec("cc");
    if (tested || match) {
        failures.push("call " + i + " of the hot functions on \"cc\": test() returned " + tested + " and exec() returned " + describe(match));
        break;
    }
}

// A group cannot be inside a group of its own name, so one group at most holds a reference.
for (var source of ["(?<x>(?<x>a))", "(?<x>a|(?<x>b))", "(?<x>a)|(?<x>(?:b|(?<x>c)))"]) {
    var error = null;
    try {
        new RegExp(source);
    } catch (e) {
        error = e;
    }
    shouldBe(error instanceof SyntaxError, true, "/" + source + "/ is a SyntaxError");
}

if (failures.length)
    throw new Error(failures.length + " case(s) failed:\n" + failures.join("\n"));
