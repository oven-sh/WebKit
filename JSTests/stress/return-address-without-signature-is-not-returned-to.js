//@ mustCrash!
//@ skip if $hostOS == "windows"
//@ runDefault("--useJIT=0")
//@ runDefault("--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=50", "--useDFGJIT=0")
//@ runDefault("--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=50", "--thresholdForOptimizeAfterWarmUp=200", "--useFTLJIT=0")
//@ runDefault("--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=50", "--thresholdForOptimizeAfterWarmUp=200", "--thresholdForFTLOptimizeAfterWarmUp=1000")

// Each run ends in the highest tier that it has. A function there whose saved return address was replaced must not return.

if (!$vm.functionsSignReturnAddress())
    $vm.crash(); // Nothing checks the return address here.

const highestTier = !$vm.useJIT() ? "LLInt" : !$vm.useDFGJIT() ? "Baseline" : $vm.useFTLJIT() ? "FTL" : "DFG";

function victim(replace)
{
    if (replace)
        $vm.removeSignatureOfReturnAddressOfCaller();
    // Asked here and not in a function of its own: the answer is about the function that asks.
    return $vm.ftlTrue() ? "FTL" : $vm.dfgTrue() ? "DFG" : $vm.baselineJITTrue() ? "Baseline" : "LLInt";
}
noInline(victim);

let reached;
for (let i = 0; i < testLoopCount; ++i)
    reached = victim(false);
if (reached !== highestTier)
    throw new Error(`ran in ${reached}, not in ${highestTier}`);

victim(true);
