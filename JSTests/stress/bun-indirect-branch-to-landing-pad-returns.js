//@ runDefault
//@ runDefault("--useJIT=0")

// What ends bun-indirect-branch-past-landing-pad-traps.js is that it calls past the landing pad: the same calls to the landing pad
// return, guarded or not.

$vm.callIntoLLInt(false);
$vm.callIntoJITCode(false);
