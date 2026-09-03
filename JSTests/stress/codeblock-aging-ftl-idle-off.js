//@ skip if !$isFTLPlatform
//@ runDefault("--useEagerCodeBlockJettisonTiming=1", "--useConcurrentJIT=0", "--optimizedCodeAgingQuietAllocationMB=0", "--optimizedCodeAgingQuietSeconds=0.02")

// With optimizedCodeAgingQuietAllocationMB=0, FTL code never ages out (the previous behaviour), even across two
// collections further apart than optimizedCodeAgingQuietSeconds with nothing allocated in between.

function shouldBe(actual, expected, msg) {
    if (actual !== expected)
        throw new Error((msg || "") + " expected " + expected + " but got " + actual);
}

var f = new Function("o", "var s = 0; for (var i = 0; i < 8; i++) s += o.a + o.b * i; return [s, isFinalTier()];");
var o = { a: 1, b: 2 }, reached = false;
for (var i = 0; i < 1e6 && !reached; i++)
    reached = f(o)[1];
if (!reached)
    throw new Error("test needs f to reach FTL");
fullGC();
var start = preciseTime();
while (preciseTime() - start < 0.04) { }
fullGC();
shouldBe(f(o)[1], true, "with the option off an FTL block never ages out;");
