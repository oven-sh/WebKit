//@ runDefault
//@ runDefault("--useSharedRegExpLiteralObjects=0")
//@ runDefault("--useSharedRegExpLiteralObjects=1")
//@ runDefault("--useSharedRegExpLiteralObjects=1", "--useJIT=0")
//@ runDefault("--useSharedRegExpLiteralObjects=1", "--useDFGJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForJITSoon=10")
//@ runDefault("--useSharedRegExpLiteralObjects=1", "--useFTLJIT=0", "--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForOptimizeAfterLongWarmUp=20")
//@ runDefault("--useSharedRegExpLiteralObjects=1", "--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForJITSoon=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForOptimizeAfterLongWarmUp=20", "--thresholdForOptimizeSoon=20", "--thresholdForFTLOptimizeAfterWarmUp=50", "--thresholdForFTLOptimizeSoon=50")
//@ runDefault("--useSharedRegExpLiteralObjects=1", "--forceOSRExitToLLInt=1", "--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForOptimizeAfterLongWarmUp=20")
//@ runDefault("--useSharedRegExpLiteralObjects=1", "--useBytecodeOptimizer=1")
//@ runBytecodeCache("--useSharedRegExpLiteralObjects=1")

// Each evaluation of a RegExp literal makes a new object. For /x/.test(s) and /x/.exec(s) the engine may hand the same object to
// the builtin every time, because nothing else can see it. Everything that could tell has to see exactly what it would have seen
// with a new object per evaluation: a different object each time, with lastIndex 0 and no properties of its own.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + expected + " but got " + actual);
}

const originalTest = RegExp.prototype.test;
const originalExec = RegExp.prototype.exec;

function isLetters(s) { return /^[a-z]+$/.test(s); }
function firstNumber(s) { return /(\d+)(x)?/.exec(s); }
function ignoreCase(s) { return /^abc$/i.test(s); }
function lettersOf(v) { return /^[a-z]+$/.test(v); }
function nested(s) { return /a/.test(s) && /b/.test(s) && !/c/.test(s); }
noInline(isLetters); noInline(firstNumber); noInline(ignoreCase); noInline(lettersOf); noInline(nested);

// 1. Results, in all tiers, including the legacy static properties and arguments that are not strings.
for (let i = 0; i < 20000; i++) {
    shouldBe(isLetters("abc"), true);
    shouldBe(isLetters("ab1"), false);
    shouldBe(ignoreCase("ABC"), true);
    shouldBe(nested("ab"), true);
    shouldBe(nested("abc"), false);
    let m = firstNumber("ab123x");
    shouldBe(m[0], "123x"); shouldBe(m[1], "123"); shouldBe(m[2], "x"); shouldBe(m.index, 2); shouldBe(m.input, "ab123x");
    shouldBe(RegExp.$1, "123"); shouldBe(RegExp.lastMatch, "123x"); shouldBe(RegExp.leftContext, "ab");
    shouldBe(firstNumber("none"), null);
    let m2 = firstNumber("7");
    shouldBe(m2[2], undefined);
    shouldBe(m === m2, false);
}
shouldBe(lettersOf(null), true);
shouldBe(lettersOf(undefined), true);
shouldBe(lettersOf(12), false);
shouldBe(lettersOf({ toString() { return "xyz"; } }), true);
try { lettersOf(Symbol()); throw new Error("no throw"); } catch (e) { shouldBe(e instanceof TypeError, true); }
try { lettersOf({ toString() { throw new Error("from toString"); } }); throw new Error("no throw"); } catch (e) { shouldBe(e.message, "from toString"); }

// 2. With g or y the literal's lastIndex is state: every evaluation starts from 0.
function globalTest(s) { return /a/g.test(s); }
function stickyExec(s) { return /a/y.exec(s); }
noInline(globalTest); noInline(stickyExec);
for (let i = 0; i < 5000; i++) {
    shouldBe(globalTest("a"), true);
    shouldBe(stickyExec("ab") !== null, true);
    shouldBe(stickyExec("ba"), null);
}

// 3. "exec" replaced while the argument of test() is being evaluated: the builtin test (already looked up) now has to call the
// replacement with the literal's object as |this|. Several activations of the same site are in flight, each must show its own object.
{
    let leaked = [];
    function reenter(depth, replace) {
        return /^[a-z]+$/.test(depth ? String(reenter(depth - 1, replace)) : (replace && (RegExp.prototype.exec = function (s) { leaked.push(this); return originalExec.call(this, s); }), "abc"));
    }
    noInline(reenter);
    // Until here nothing in this realm has touched RegExp.prototype: this is the first time, and it happens in the middle of four calls.
    for (let i = 0; i < 3000; i++)
        shouldBe(reenter(3, false), true);
    shouldBe(reenter(3, true), true);
    shouldBe(leaked.length, 4);
    shouldBe(new Set(leaked).size, 4, "one object per evaluation");
    for (let r of leaked) {
        shouldBe(r instanceof RegExp, true);
        shouldBe(Object.getPrototypeOf(r), RegExp.prototype);
        shouldBe(r.lastIndex, 0);
        shouldBe(r.source, "^[a-z]+$");
        shouldBe(r.flags, "");
        shouldBe(Object.getOwnPropertyNames(r).join(), "lastIndex");
        // and it is an ordinary, usable, extensible object
        r.expando = 1;
        r.lastIndex = 5;
        r.compile("z");
        shouldBe(r.source, "z");
    }
    RegExp.prototype.exec = originalExec;
    // The objects that leaked were modified: later evaluations must not see any of it.
    for (let i = 0; i < 300; i++) {
        shouldBe(isLetters("abc"), true);
        shouldBe(reenter(1, false), true);
    }
}

// 4. "test" replaced: |this| is the literal's object, a different one each time.
{
    let seen = [];
    RegExp.prototype.test = function (s) { seen.push(this); this.mark = seen.length; return originalTest.call(this, s); };
    for (let i = 0; i < 200; i++)
        shouldBe(isLetters("abc"), true);
    shouldBe(new Set(seen).size, 200);
    shouldBe(seen.every((r, i) => r.mark === i + 1 && r.lastIndex === 0), true);
    RegExp.prototype.test = originalTest;
    for (let i = 0; i < 200; i++)
        shouldBe(isLetters("abc"), true);
    shouldBe(seen.length, 200);
}

// 5. "test" as an accessor, on the prototype and then further up.
{
    let receivers = [];
    Object.defineProperty(RegExp.prototype, "test", { get() { receivers.push(this); return originalTest; }, configurable: true });
    for (let i = 0; i < 100; i++)
        shouldBe(isLetters("abc"), true);
    shouldBe(new Set(receivers).size, 100);
    delete RegExp.prototype.test;
    Object.prototype.test = function (s) { receivers.push(this); return "from Object.prototype"; };
    shouldBe(isLetters("abc"), "from Object.prototype");
    shouldBe(receivers.length, 101);
    delete Object.prototype.test;
    Object.defineProperty(RegExp.prototype, "test", { value: originalTest, writable: true, configurable: true });
    shouldBe(isLetters("abc"), true);
}

// 6. exec replaced for good, and as an accessor.
{
    let seen = [];
    RegExp.prototype.exec = function (s) { seen.push(this); return originalExec.call(this, s); };
    for (let i = 0; i < 100; i++) {
        shouldBe(firstNumber("a1")[0], "1");
        shouldBe(isLetters("abc"), true);
    }
    shouldBe(new Set(seen).size, 200);
    RegExp.prototype.exec = originalExec;
}

// 7. Another realm has its own literals, prototypes and watchpoints.
{
    let other = createGlobalObject();
    let otherIsLetters = other.eval("(function (s) { return /^[a-z]+$/.test(s); })");
    for (let i = 0; i < 5000; i++)
        shouldBe(otherIsLetters("abc"), true);
    let otherSeen = [];
    other.RegExp.prototype.test = function (s) { otherSeen.push(this); return true; };
    shouldBe(otherIsLetters("123"), true);
    shouldBe(otherIsLetters("123"), true);
    shouldBe(otherSeen.length, 2);
    shouldBe(otherSeen[0] === otherSeen[1], false);
    shouldBe(otherSeen[0] instanceof other.RegExp, true);
}

// 8. Survives collections; functions that come and go.
for (let i = 0; i < 50; i++) {
    let f = new Function("s", "return /q" + (i % 5) + "/.test(s);");
    for (let j = 0; j < 50; j++)
        shouldBe(f("q" + (i % 5)), true);
    if (i % 10 === 0)
        fullGC();
    shouldBe(f("zz"), false);
}
gc();
shouldBe(isLetters("abc"), true);
