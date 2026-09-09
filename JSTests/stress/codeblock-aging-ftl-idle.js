//@ skip if !$isFTLPlatform
//@ runDefault("--useEagerCodeBlockJettisonTiming=1", "--useConcurrentJIT=0", "--optimizedCodeAgingQuietSeconds=0.02")

// FTL code has no execution counter of its own, so it ages against the mutator as a whole: it is
// jettisoned only by a full collection the embedder tagged idle (idleFullGC() here, GCRequest::isIdle), and
// only once no collection has seen the mutator allocating for optimizedCodeAgingQuietSeconds. Ordinary
// collections never drop it; allocation keeps it (and the baseline code it pins) alive.

function shouldBe(actual, expected, msg) {
    if (actual !== expected)
        throw new Error((msg || "") + " expected " + expected + " but got " + actual);
}

function makeHot() {
    return new Function("o", "var s = 0; for (var i = 0; i < 8; i++) s += o.a + o.b * i; return [s, isFinalTier()];");
}

function warmToFTL(f) {
    var o = { a: 1, b: 2 };
    for (var i = 0; i < 1e6; i++) {
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

// optimizedCodeAgingQuietSeconds above; leave some slack for ApproximateTime.
var quiet = 0.04;
var o = { a: 1, b: 2 };

// 1. Nothing is allocated between two full collections a quiet window apart. Ordinary full collections
//    (allocation-paced, forced, memory pressure) never let the block go; one tagged idle does.
var f = makeHot();
if (!warmToFTL(f))
    throw new Error("test needs f to reach FTL");
fullGC();
idleFor(quiet);
fullGC();          // quiet and past the window, but not an idle collection: kept
idleFor(quiet);
fullGC();
shouldBe(f(o)[1], true, "an FTL block should survive full collections that are not tagged idle;");
allocateMB(4);
var activeAt = preciseTime();
fullGC();          // this collection saw the mutator allocating: the clock restarts here
idleFullGC();      // idle, but the mutator was active a moment ago: kept
// (unless the two collections themselves took longer than the quiet window, e.g. on a slow debug build)
if (preciseTime() - activeAt < 0.02)
    shouldBe(f(o)[1], true, "an FTL block should survive an idle collection right after an active one;");
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
