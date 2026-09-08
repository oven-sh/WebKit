//@ skip if !$jitTests
//@ runDefault("--useConcurrentJIT=0", "--startupJITDeferralScale=8", "--startupJITDeferralMaxMs=0", "--thresholdForJITAfterWarmUp=500", "--thresholdForJITSoon=500", "--useDFGJIT=0")

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ': bad value: ' + actual);
}

// Each LLInt call adds 25 to the execution counter (15 prologue + 10 epilogue); the normal
// LLInt->Baseline threshold is 500 (20 calls), so inside an 8x window it is 4000 (160 calls).
function probe() { return $vm.llintTrue(); }
noInline(probe);

if ($vm.useJIT()) {
    let last;
    for (let i = 0; i < 60; ++i)
        last = probe();
    shouldBe(last, true, "well past the normal threshold but inside the startup window");

    shouldBe($vm.endStartupJITDeferral(), true, "window was active");
    shouldBe($vm.endStartupJITDeferral(), false, "idempotent");

    // Deferred counters re-check within max(threshold, maximumExecutionCountsBetweenCheckpoints) = 1000 counts = 40 calls.
    for (let i = 0; i < 60; ++i)
        last = probe();
    shouldBe(last, false, "compiled promptly once the window ended");
}
