//@ runDefault("--useConcurrentJIT=0", "--thresholdForOptimizeAfterWarmUp=10", "--thresholdForFTLOptimizeAfterWarmUp=20")
// An empty /u match followed by a surrogate pair: every replace loop must step over
// the whole pair, since a /u match never starts mid-pair. The remove-all fast path
// (empty replacement) and DFG's constant folding of String.prototype.replace used to
// step one code unit and re-find the same empty match forever.
function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error("expected " + JSON.stringify(expected) + " but got " + JSON.stringify(actual));
}

shouldBe("\u{1F600} x".replace(/\s*/gu, ""), "\u{1F600}x");
shouldBe("\u{1F600}\u{1F600}x".replace(/(?:)/gu, ""), "\u{1F600}\u{1F600}x");
shouldBe("ab\u{1F600} x".replace(/[a-c]*/gv, ""), "\u{1F600} x");
shouldBe("\u{1F600} x".replace(/\s*/gu, "-"), "-\u{1F600}--x-");
shouldBe("x\u{1F600}".replace(/(?<=x)|\B/gu, ""), "x\u{1F600}");
shouldBe("\uD83D😀".replace(/(?:)/gu, "."), ".\uD83D.😀.");

// DFG constant folding of replace on constant operands (both empty and non-empty replacement).
function foldedEmpty() { return "\u{1F600} x".replace(/\s*/gu, ""); }
function foldedDash() { return "\u{1F600} x".replace(/\s*/gu, "-"); }
function foldedCallbackFree() { return "a\u{10428}\u{10428}b".replace(/\B|/gu, "|"); }
for (let i = 0; i < 2000; ++i) {
    shouldBe(foldedEmpty(), "\u{1F600}x");
    shouldBe(foldedDash(), "-\u{1F600}--x-");
    shouldBe(foldedCallbackFree(), "|a|\u{10428}|\u{10428}|b|");
}
