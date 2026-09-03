//@ skip if !$isFTLPlatform
//@ runDefault("--useEagerCodeBlockJettisonTiming=1", "--useConcurrentJIT=0", "--optimizedCodeAgingQuietSeconds=0.02")

// FTL code has no execution counter of its own, so it ages against the mutator as a whole
// (Options::optimizedCodeAgingQuietAllocationMB): once past its TTL it is jettisoned by a full
// collection that finds (almost) nothing was allocated since the previous one looked at it, and
// kept - together with the baseline code it pins - whenever the mutator has been allocating.

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

// 1. Nothing is allocated between two full collections a TTL apart: the FTL block goes.
var f = makeHot();
if (!warmToFTL(f))
    throw new Error("test needs f to reach FTL");
fullGC();          // records the allocation total for f's FTL block
idleFor(quiet);
fullGC();          // nothing allocated since, past TTL: aged out
shouldBe(f(o)[1], false, "an FTL block should age out across an idle stretch;");
shouldBe(f(o)[0], 64, "and the function should still work;");

// 2. The mutator allocates between the collections: the block stays, however old.
var g = makeHot();
if (!warmToFTL(g))
    throw new Error("test needs g to reach FTL");
fullGC();
for (var round = 0; round < 3; round++) {
    idleFor(quiet);
    allocateMB(4);
    fullGC();
    shouldBe(g(o)[1], true, "an FTL block should survive while the mutator is allocating (round " + round + ");");
}

// 3. And once the mutator goes quiet it goes too.
idleFor(quiet);
fullGC();
idleFor(quiet);
fullGC();
shouldBe(g(o)[1], false, "the FTL block should age out once the mutator goes quiet;");
