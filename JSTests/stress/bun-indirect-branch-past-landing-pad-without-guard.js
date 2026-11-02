//@ runDefault("--useBranchTargetIdentification=0")
//@ runDefault("--useBranchTargetIdentification=0", "--useJIT=0")

// What bun-indirect-branch-past-landing-pad-traps.js does ends it because of the guard: without the guard it does no harm.

if ($vm.pagesOfLLIntAreGuarded() || $vm.pagesOfJITCodeAreGuarded())
    throw new Error("The option did not take the guard away.");

for (const call of [$vm.callIntoLLInt, $vm.callIntoJITCode]) {
    call(false);
    call(true);
}
