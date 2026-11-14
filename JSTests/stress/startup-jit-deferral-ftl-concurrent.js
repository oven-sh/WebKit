//@ skip if !$jitTests
//@ runDefault("--useConcurrentJIT=1", "--startupJITDeferralScale=8", "--thresholdForJITAfterWarmUp=20", "--thresholdForJITSoon=20", "--thresholdForOptimizeAfterWarmUp=40", "--thresholdForOptimizeSoon=40", "--thresholdForFTLOptimizeAfterWarmUp=1000", "--thresholdForFTLOptimizeSoon=1000")

// With the concurrent JIT, a finished FTL compile forces the DFG tier-up slow path so that the next
// call installs it. Inside a startup deferral window that slow path must still install the finished
// code at once, instead of treating the forced check as one more deferred crossing.

function probe() { return $vm.ftlTrue() ? "ftl" : $vm.dfgTrue() ? "dfg" : "lower"; }
noInline(probe);

function warm() { return "warm"; }
noInline(warm);

// Up to n rounds of: let the compiler thread work, then call fn 5 times. Stops once fn returns tier.
function rounds(n, fn, tier) {
    let last = "lower";
    let calls = 0;
    for (let r = 0; r < n && last !== tier; ++r) {
        sleepSeconds(0.01);
        for (let i = 0; i < 5; ++i) {
            last = fn();
            ++calls;
        }
    }
    return { last, calls };
}

// A tier-up slow path of any code block installs every finished plan of the VM. Get the driver to the
// baseline JIT before probe runs, and keep it out of the DFG, so that afterwards only probe's own slow
// path can install probe's FTL code.
noDFG(rounds);
noDFG(warm);
rounds(100, warm, "none");

if ($vm.useJIT() && $vm.useFTLJIT()) {
    // The lower tiers compile asynchronously too. Wait until the DFG code runs.
    const dfg = rounds(2000, probe, "dfg");
    if (dfg.last !== "dfg")
        throw new Error("never reached the DFG: " + dfg.last);

    // Each DFG return adds 15 to the FTL tier-up counter, and the threshold here is 1000 counts scaled
    // by the bytecode cost (1379). Inside the 8x window the compile is queued at 11032 counts (736
    // calls) and finishes during the next sleep. The forced slow path must then install it: a check
    // that defers instead re-arms the counter for 643 more calls, past the 1300 calls allowed here.
    const ftl = rounds(260, probe, "ftl");
    if (ftl.last !== "ftl")
        throw new Error("the finished FTL compile was not installed within " + ftl.calls + " calls");
}
