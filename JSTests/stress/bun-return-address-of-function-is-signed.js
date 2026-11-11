//@ runDefault("--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=50", "--thresholdForOptimizeAfterWarmUp=200", "--thresholdForFTLOptimizeAfterWarmUp=1000")
//@ runDefault("--useJIT=0")

// Where functions are to sign the return address that they save, they do so in every tier: a frame that one tier made is
// returned from by another.

// Where they are not to, or the processor has no pointer authentication, there is nothing to ask.
if (!$vm.functionsSignReturnAddress())
    quit();

const seen = { };

function check()
{
    // Asked here and not in a function of its own: the answer is about the function that asks.
    let name = $vm.ftlTrue() ? "FTL" : $vm.dfgTrue() ? "DFG" : $vm.baselineJITTrue() ? "Baseline" : "LLInt";
    if (!$vm.returnAddressOfCallerIsSigned())
        throw new Error(`${name}: the return address is not signed`);
    seen[name] = true;
}
noInline(check);

for (let i = 0; i < testLoopCount; ++i)
    check();

function shouldHaveSeen(name, enabled)
{
    if (enabled && !seen[name])
        throw new Error(`never ran in ${name}`);
}
shouldHaveSeen("LLInt", true);
shouldHaveSeen("Baseline", $vm.useJIT());
shouldHaveSeen("DFG", $vm.useDFGJIT());
shouldHaveSeen("FTL", $vm.useDFGJIT() && $vm.useFTLJIT());
