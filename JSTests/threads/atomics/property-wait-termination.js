//@ requireOptions("--useJSThreads=1", "--watchdog=500", "--watchdog-exception-ok")
// API-I24 (termination half; skippable per §6, exercised here via the shell
// watchdog): a property Atomics.wait interrupted by a termination request
// must observe it within a 10ms poll quantum (5.6-4: the per-waiter
// condition wait polls vm.hasTerminationRequest() because VMTraps cannot
// wake PWT waiters) and throw the termination exception (5.6-7 / 4.5) —
// NEVER return "ok" or "timed-out".
//
// Mechanics: the waiter parks forever (infinite timeout, nobody notifies);
// main blocks in join(). The watchdog fires at 500ms and requests
// termination; the waiter's quantum poll sees it, sets Terminated, and
// throwTerminationException() unwinds fn; the completion sequence publishes
// the Failed result and wakes the joiner, whose rethrow leaves the script as
// an uncaught termination — which --watchdog-exception-ok maps to exit 0.
//
// Failure modes this catches:
// - wait returns a string instead of terminating  => FAILURE print + throw
//   (non-watchdog Error => nonzero exit even with --watchdog-exception-ok);
// - termination never observed (poll missing)     => the run HANGS (the
//   runner/amplifier timeout reports it).
load("../harness.js", "caller relative");

const o = { k: 0 };

const t = new Thread(() => {
    const r = Atomics.wait(o, "k", 0); // infinite timeout; never notified
    // Unreachable unless I24 is violated:
    print("FAILURE: property Atomics.wait returned '" + r + "' under termination");
    throw new Error("API-I24 violated: wait returned " + r);
});

t.join();

// Unreachable unless the join swallowed the termination:
print("FAILURE: join() returned normally under termination");
throw new Error("API-I24 violated: join returned normally");
