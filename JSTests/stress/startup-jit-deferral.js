//@ skip if !$jitTests
//@ runDefault("--useConcurrentJIT=0", "--startupJITDeferralScale=8", "--startupJITDeferralMaxMs=0", "--thresholdForJITAfterWarmUp=500", "--thresholdForJITSoon=500", "--useDFGJIT=0")

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ': bad value: ' + actual);
}

// Each LLInt call adds 15 to the execution counter (5 prologue + 10 epilogue); the normal
// LLInt->Baseline threshold is 500 (34 calls), so inside an 8x window it is 4000 (267 calls).
function probe() { return $vm.llintTrue(); }
noInline(probe);
function probe2() { return $vm.llintTrue(); }
noInline(probe2);

if ($vm.useJIT()) {
    let last;
    for (let i = 0; i < 60; ++i)
        last = probe();
    shouldBe(last, true, "well past the normal threshold but inside the startup window");

    shouldBe($vm.endStartupJITDeferral(), true, "window was active");
    shouldBe($vm.endStartupJITDeferral(), false, "idempotent");

    // Deferred counters re-check at most maximumExecutionCountsBetweenCheckpoints = 1000 counts after the deferred check (here 600 counts = 40 calls away).
    for (let i = 0; i < 60; ++i)
        last = probe();
    shouldBe(last, false, "compiled promptly once the window ended");

    // Re-arming after the window ended defers a fresh function again, with no deadline.
    $vm.setStartupJITDeferralScale(8);
    for (let i = 0; i < 60; ++i)
        last = probe2();
    shouldBe(last, true, "re-armed window defers tier-up");

    $vm.setStartupJITDeferralScale(1);
    shouldBe($vm.endStartupJITDeferral(), false, "scale 1 ended the window");
    for (let i = 0; i < 60; ++i)
        last = probe2();
    shouldBe(last, false, "compiled promptly once the scale returned to 1");
}
