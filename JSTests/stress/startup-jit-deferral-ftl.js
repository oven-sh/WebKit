//@ skip if !$jitTests
//@ runDefault("--useConcurrentJIT=0", "--startupJITDeferralScale=8", "--thresholdForJITAfterWarmUp=20", "--thresholdForJITSoon=20", "--thresholdForOptimizeAfterWarmUp=40", "--thresholdForOptimizeSoon=40", "--thresholdForFTLOptimizeAfterWarmUp=1000", "--thresholdForFTLOptimizeSoon=1000")

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ': bad value: ' + actual);
}

// The lower tiers are set to tier up within a few calls even inside the 8x window, so the probe
// reaches the DFG early. Each DFG return adds 15 to the FTL tier-up counter; the normal DFG->FTL
// threshold is 1000 (67 calls), so inside an 8x window it is 8000 (534 calls).
function probe() { return $vm.ftlTrue() ? "ftl" : $vm.dfgTrue() ? "dfg" : "lower"; }
noInline(probe);

if ($vm.useJIT() && $vm.useFTLJIT()) {
    let last;
    for (let i = 0; i < 250; ++i)
        last = probe();
    shouldBe(last, "dfg", "well past the normal FTL threshold but inside the startup window");

    $vm.setStartupJITDeferralScale(1);

    // The deferred counter re-checks within what is left of the scaled period (at most 7000 counts, 467
    // calls), and the compile follows at once.
    for (let i = 0; i < 1500; ++i)
        last = probe();
    shouldBe(last, "ftl", "FTL-compiled promptly once the window ended");
}
