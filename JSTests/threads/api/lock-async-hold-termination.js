//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--watchdog=300", "--watchdog-exception-ok")
// A termination raised inside an asyncHold(fn) delivered fn must stay pending
// when the settle task returns, so the settling run-loop turn aborts
// (DeferredWorkTimer::doWork forbids execution and stops the run loop). The
// asyncHold promise is neither resolved nor rejected: no reaction handler may
// run on the terminated thread, and JS must not be able to catch the
// termination through the promise.
//
// Mechanics: the settle task runs on the run loop the shell pumps after this
// script; fn spins until the 300ms watchdog terminates it. Before the fix the
// termination was caught into a rejection and the handler below ran; now the
// shell exits through the watchdog path, which --watchdog-exception-ok maps
// to success.
const lock = new Lock();
lock.asyncHold(() => {
    for (;;) { }
}).then(
    () => {
        print("FAILURE: asyncHold promise resolved after termination");
        $vm.crash();
    },
    (e) => {
        print("FAILURE: asyncHold promise rejected after termination: " + e);
        $vm.crash();
    });
