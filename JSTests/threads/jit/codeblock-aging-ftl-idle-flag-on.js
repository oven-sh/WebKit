//@ requireOptions("--useJSThreads=1", "--useEagerCodeBlockJettisonTiming=1", "--optimizedCodeAgingQuietSeconds=0.2")
// The new base's idle aging of optimized code (stress/codeblock-aging-ftl-idle.js), with the flag on. A call-link
// record used to pin the CodeBlock it names for as long as the record lived, so a function linked from a live caller
// kept its FTL code whatever the idle collections decided; the record now names it weakly, like upstream's call
// links, and the End phase that finds it dead clears the record (SPEC-jit history §43).
//
// The same steps as the upstream test with its quiet window ten times longer (0.2 s instead of 0.02 s), minus its check
// that an idle collection right after an active one keeps the block: that check races the collections themselves -
// flag off it failed 10 of 200 runs under the race amplifier and the flag-on copy 1 of 200 even with the longer window
// - and it tests upstream's aging heuristic, not the call-link records; the upstream test keeps it flag off. The flag
// forces concurrent compilation, so the warm-up polls until the FTL code is installed.

function shouldBe(actual, expected, msg) {
    if (actual !== expected)
        throw new Error((msg || "") + " expected " + expected + " but got " + actual);
}

function makeHot() {
    return new Function("o", "var s = 0; for (var i = 0; i < 8; i++) s += o.a + o.b * i; return [s, isFinalTier()];");
}

function warmToFTL(f) {
    var o = { a: 1, b: 2 };
    for (var i = 0; i < 1e7; i++) {
        if (f(o)[1])
            return true;
    }
    return false;
}

function idleFor(seconds) {
    var start = preciseTime();
    while (preciseTime() - start < seconds) { }
}

function allocateMB(n) {
    var keep = [];
    for (var i = 0; i < n * 16; i++)
        keep.push(new Float64Array(8192)); // 64 KB each
    return keep.length;
}

var quiet = 0.4;  // past the window, with slack for ApproximateTime
var o = { a: 1, b: 2 };

// 1. Nothing is allocated between two full collections a quiet window apart. Ordinary full collections never let
//    the block go; one tagged idle does.
var f = makeHot();
if (!warmToFTL(f))
    throw new Error("test needs f to reach FTL");
fullGC();
idleFor(quiet);
fullGC();
idleFor(quiet);
fullGC();
shouldBe(f(o)[1], true, "an FTL block should survive full collections that are not tagged idle;");
allocateMB(4);
fullGC();          // this collection saw the mutator allocating: the clock restarts here
idleFor(quiet);
idleFullGC();      // idle and nothing has happened for the quiet window: aged out
shouldBe(f(o)[1], false, "an FTL block should age out in an idle collection after a quiet stretch;");
shouldBe(f(o)[0], 64, "and the function should still work;");

// 2. The mutator allocates between the collections: the block stays, however old.
var g = makeHot();
if (!warmToFTL(g))
    throw new Error("test needs g to reach FTL");
fullGC();
for (var round = 0; round < 3; round++) {
    idleFor(quiet);
    allocateMB(4);
    idleFullGC();
    shouldBe(g(o)[1], true, "an FTL block should survive while the mutator is allocating (round " + round + ");");
}

// 3. And once the mutator goes quiet it goes too.
idleFor(quiet);
idleFullGC();
idleFor(quiet);
idleFullGC();
shouldBe(g(o)[1], false, "the FTL block should age out once the mutator goes quiet;");
