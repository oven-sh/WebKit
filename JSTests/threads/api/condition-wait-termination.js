//@ requireOptions("--useJSThreads=1", "--watchdog=500", "--watchdog-exception-ok")
// Landed deviation D9 (docs/threads/INTEGRATE-api.md): Condition.prototype.wait
// parks in 10ms ParkingLot quanta and polls vm.hasTerminationRequest()
// between parks — the same termination-poll rule SPEC-api 5.6-4 mandates for
// property Atomics.wait (VMTraps cannot wake either kind of waiter). Without
// the poll, a cond.wait whose notifier never arrives (or was itself
// terminated and unwound without notifying) parks forever and the watchdog
// cannot kill the run.
//
// Mechanics: the main thread takes the lock and waits on the condition with
// no notifier anywhere. The watchdog fires at 500ms and requests
// termination; the wait's quantum poll observes it and throws the
// termination exception WITHOUT reacquiring the lock (the enclosing hold's
// epilogue guard then skips its release, same shape as a 4.3(a) consumed
// hold); the uncaught termination maps to exit 0 under
// --watchdog-exception-ok.
//
// Failure modes this catches:
// - cond.wait returns under termination   => FAILURE print + plain Error
//   (nonzero exit even with --watchdog-exception-ok);
// - termination never observed (poll missing) => the run HANGS (the
//   runner/amplifier timeout reports it).
load("../harness.js", "caller relative");

const lock = new Lock();
const cond = new Condition();

lock.hold(() => {
    cond.wait(lock); // never notified; only termination can end this
    // Unreachable unless the D9 poll mis-reports:
    print("FAILURE: cond.wait returned under termination");
    throw new Error("D9 violated: cond.wait returned");
});

// Unreachable unless hold swallowed the termination:
print("FAILURE: hold returned normally under termination");
throw new Error("D9 violated: hold returned normally");
