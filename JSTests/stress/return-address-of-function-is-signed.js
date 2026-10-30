//@ runDefault("--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=50", "--thresholdForOptimizeAfterWarmUp=200", "--thresholdForFTLOptimizeAfterWarmUp=1000")
//@ runDefault("--useJIT=0")

// Where functions are to sign the return address that they save, they do so in every tier: a frame that one tier made is
// returned from by another.

const expected = $vm.functionsSignReturnAddress();
const seen = { };

function check()
{
    // Asked here and not in a function of its own: the answer is about the function that asks.
    let name = $vm.ftlTrue() ? "FTL" : $vm.dfgTrue() ? "DFG" : $vm.baselineJITTrue() ? "Baseline" : "LLInt";
    if ($vm.returnAddressOfCallerIsSigned() !== expected)
        throw new Error(`${name}: the return address is ${expected ? "not " : ""}signed`);
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
