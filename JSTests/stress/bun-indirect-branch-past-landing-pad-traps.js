//@ mustCrash!
//@ skip if $hostOS == "windows"
//@ runDefault("--useJIT=0")
//@ runDefault

// An indirect branch into LLInt, or into the code of the JITs, must land on a landing pad. The code that is called here is a landing
// pad and a return, so past the landing pad it does no harm where nothing is guarded: what ends this is the guard and nothing else.

const intoJITCode = $vm.useJIT();
const call = intoJITCode ? $vm.callIntoJITCode : $vm.callIntoLLInt;

if (!(intoJITCode ? $vm.pagesOfJITCodeAreGuarded() : $vm.pagesOfLLIntAreGuarded()))
    $vm.crash(); // Nothing is guarded here.

call(false);
print("It returns from the landing pad.");
call(true);
quit(); // Not a crash, so the test fails.
