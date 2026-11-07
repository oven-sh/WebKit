//@ runDefault("--useDollarVM=1", "--useJIT=0", "--watchdog=600000")

// $vm.callWithTerminationAtTrapCheck() counts trap checks with the trap bit of the watchdog. A VM that has a watchdog
// refuses the call: the watchdog would get none of its checks while the count runs.

let error;
let started = false;
try {
    $vm.callWithTerminationAtTrapCheck(() => { started = true; }, 1);
} catch (e) {
    error = e;
}
if (!(error instanceof TypeError) || error.message != "$vm.callWithTerminationAtTrapCheck() needs a VM without a watchdog")
    throw new Error(`expected the TypeError of the refusal, got ${error}`);
if (started)
    throw new Error("the function started");
if ($vm.hasPendingTermination())
    throw new Error("something is still pending");
