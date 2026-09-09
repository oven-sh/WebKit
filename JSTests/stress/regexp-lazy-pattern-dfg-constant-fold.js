//@ runDefault("--useConcurrentJIT=1", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=100", "--thresholdForFTLOptimizeAfterWarmUp=1000")
//@ runDefault("--useConcurrentJIT=1", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=100", "--thresholdForFTLOptimizeAfterWarmUp=1000", "--useLazyRegExpPatternConstruction=false")

// DFG strength reduction constant-folds RegExpExec/RegExpTest/RegExpMatchFast on constant subjects by
// calling RegExp::matchConcurrently on the compiler thread, and Graph::regExpFirstCharacterBitmap builds a
// first-character filter there. With lazily constructed patterns (longer than 64 characters) the
// YarrPattern for the RegExp cell is first built at compile time; that must stay on the mutator, and a
// RegExp that was never executed before its function got DFG-compiled must still give right answers.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": expected " + expected + " but got " + actual);
}

let filler = "(?:" + "q".repeat(70) + ")?";

// Hot: executed every iteration, so it is compiled on the mutator before the DFG looks at it.
function hotExec(s) {
    return /complex(pat)tern\p{L}+(?:zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz)?/u.exec(s);
}

// The regexes in the `rare` arm are never executed while the function warms up and gets DFG compiled.
// The test/exec/search call sites live in small helpers that are hot (so their call link infos know the
// RegExp.prototype callee and the DFG inlines them as RegExpTest/RegExpExec/RegExpSearch), while the
// RegExp operand in the rare arm is a NewRegexp constant whose cell has no code and no YarrPattern yet.
function doTest(re, s) { return re.test(s); }
function doExec(re, s) { return re.exec(s); }
function doSearch(s, re) { return s.search(re); }

function coldFold(i, takeRare) {
    let r = i & 1;
    if (doTest(/^[01](?:tttttttttttttttttttttttttttttttttttttttttttttttttttttttttttttttttttttttttttt)?/, "" + r))
        r += 0;
    doExec(/hot(exec)/, "hotexec");
    doSearch("hotsearch", /sea/);
    if (takeRare) {
        if (doTest(/^never(ran)before[a-z]+\d{2,}(?:yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy)?$/, "neverranbeforeabc123"))
            r += 10;
        let m = doExec(/(alpha)(beta)?(gamma)\s+(?:wwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwww)?(\w+)/, "alphagamma   tail");
        if (m)
            r += m.length * 100 + (m[2] === undefined ? 1000 : 0);
        r += doSearch("xxabcabcyy", /abc(?:vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv)?abc/) * 10000;
        // Sticky + first-character bitmap path.
        let sticky = /[abc]def(?:uuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuu)?/y;
        sticky.lastIndex = 1;
        if (doTest(sticky, "xadef"))
            r += 100000;
    }
    return r;
}

// RegExp objects created from long dynamic strings, tested against a non-constant subject in a hot loop.
let dynamicSources = [
    "(a)(?:b)(c)(" + filler + ")?|(d)",
    "^(?:(?=abc)\\w{3}" + filler + "|z)",
    "(?<!x)abc(?!y)" + filler,
    "(a|(b|(c|(d" + filler + "))))",
    "\\p{Script=Greek}+" + filler,
    "[\\p{L}--[a-z]]+" + filler,
];
let dynamicFlags = ["", "", "", "g", "u", "v"];
let dynamic = dynamicSources.map((s, i) => new RegExp(s, dynamicFlags[i]));

function hotDynamic(k, s) {
    let re = dynamic[k];
    re.lastIndex = 0;
    return re.test(s);
}

let subjects = ["complexpatternéabc", "nope", "abc", "d", "zzz", "αβ", "ABC"];
for (let i = 0; i < 5000; ++i) {
    let m = hotExec(subjects[i % 2 ? 0 : 1]);
    if (i % 2)
        shouldBe(m[1], "pat", "hotExec capture");
    else
        shouldBe(m, null, "hotExec miss");
    shouldBe(coldFold(i, false), i & 1, "coldFold common arm");
    hotDynamic(i % dynamic.length, subjects[i % subjects.length]);
}

// Now take the rare arm: whatever the DFG folded (or did not) must agree with the interpreter.
let expected = 1 + 10 + 5 * 100 + 1000 + 2 * 10000 + 100000;
for (let i = 0; i < 200; ++i)
    shouldBe(coldFold(1, true), expected, "coldFold rare arm");

shouldBe(dynamic[0].exec("abc").length, 5, "dynamic[0] captures");
shouldBe(dynamic[1].test("abc"), true, "dynamic[1]");
shouldBe(dynamic[2].test("xabc"), false, "dynamic[2] lookbehind");
dynamic[3].lastIndex = 0; // 'g'
shouldBe(dynamic[3].exec("c")[3], "c", "dynamic[3] nested");
shouldBe(dynamic[4].test("αβ"), true, "dynamic[4] unicode property");
shouldBe(dynamic[5].test("ABC"), true, "dynamic[5] unicode sets");
shouldBe(dynamic[5].test("abc"), false, "dynamic[5] unicode sets subtraction");
