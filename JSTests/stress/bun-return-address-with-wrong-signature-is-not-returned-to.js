//@ mustCrash!
//@ skip if $hostOS == "windows"
//@ runDefault("--useJIT=0")
//@ runDefault("--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=50", "--useDFGJIT=0")
//@ runDefault("--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=50", "--thresholdForOptimizeAfterWarmUp=200", "--useFTLJIT=0")
//@ runDefault("--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=50", "--thresholdForOptimizeAfterWarmUp=200", "--thresholdForFTLOptimizeAfterWarmUp=1000")

// Each run ends in the highest tier that it has. A function there whose saved return address has the wrong signature must not
// return.

if (!$vm.functionsSignReturnAddress())
    $vm.crash(); // Nothing checks the return address here.

const highestTier = !$vm.useJIT() ? "LLInt" : !$vm.useDFGJIT() ? "Baseline" : $vm.useFTLJIT() ? "FTL" : "DFG";
const corrupt = $vm.corruptSignatureOfReturnAddressOfCaller;

// So many that the DFG and the FTL expect no callee in particular. Code that has never run, or a callee that they did not
// expect, makes them exit to Baseline in front of the call, and then it is Baseline that returns.
const harmless = [];
for (let i = 0; i < 32; ++i)
    harmless.push(new Function(`return ${i};`));

function victim(callee)
{
    callee();
    // Asked here and not in a function of its own: the answer is about the function that asks. And behind the call.
    let tier = $vm.ftlTrue() ? "FTL" : $vm.dfgTrue() ? "DFG" : $vm.baselineJITTrue() ? "Baseline" : "LLInt";
    if (tier !== highestTier && callee === corrupt)
        corrupt(); // Puts the signature right again: the run ends without a crash, and so fails.
    return tier;
}
noInline(victim);

let reached;
for (let i = 0; i < testLoopCount; ++i)
    reached = victim(harmless[i % harmless.length]);
if (reached !== highestTier)
    throw new Error(`ran in ${reached}, not in ${highestTier}`);

reached = victim(corrupt);
throw new Error(`returned, from ${reached}`);
