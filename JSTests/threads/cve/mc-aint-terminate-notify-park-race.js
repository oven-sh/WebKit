//@ requireOptions("--useJSThreads=1", "--watchdog=500", "--watchdog-exception-ok")
// MC-AINT S3 (docs/threads/cve/map-MC-AINT.md): the W1 service-vs-notify
// revoke race — r15 F2 disposition (a), recorded as a caller-side
// obligation at fireTerminationVMWideAfterParkedCarrierService
// (runtime/VMTraps.cpp:963-986 CAVEAT).
//
// Mechanism: a parked CARRIER observes NeedWatchdogCheck at a D9 quantum,
// runs the §J.3 reacquisition and services Watchdog::shouldTerminate on its
// own thread; a terminate verdict pre-sets the consumed-by-carrier shield
// on the premise that this park FAILS per SD8/§E.5. A racing notify that
// dequeues the parked waiter DURING the service window falsifies the
// premise: the park completes "ok" without servicing the termination, and
// unless the park site revokes (re-raises fireTrapVMWide(NeedTermination)),
// the shield lets the host's clear-and-re-enter swallow the termination —
// the lost-abort variant of asynchronous-interruption-at-an-unsafe-point.
// Current revokes live in waitSyncWithPerWaitNode (WaiterListManager.cpp)
// and ConditionObject's wait loop; this test exists so that obligation
// cannot rot silently.
//
// Shape: the MAIN thread (the carrier — W1 is carrier-only, annex W) parks
// repeatedly in property Atomics.wait while a spawned notifier storms
// notify on the same (cell, key): every watchdog-check episode on the
// parked carrier races a dequeue. The watchdog fires at 500ms with the
// default terminate verdict.
//
// Oracle (API-I24 + TERM1 delivery):
//  - the run must END TERMINATED: --watchdog-exception-ok maps the uncaught
//    termination to exit 0; reaching the tail prints FAILURE and throws an
//    ordinary Error (nonzero exit even with the flag);
//  - a LOST termination (shield not revoked in disposition (a)) presents as
//    the carrier re-parking forever after the watchdog already decided
//    terminate => the run HANGS; the runner/amplifier timeout reports it;
//  - wait must never return a value other than "ok"/"not-equal"/"timed-out"
//    pre-termination (5.6 surface unchanged by the race).
//
// Deterministic-leaning: the notify storm makes disposition (a) windows
// frequent rather than rare, but the exact interleaving is scheduler-owned,
// so the test is also amplifier-ready. Valid under the phase-1 GIL (the W1
// split is GIL-off-only, but the GIL-on folded predicate must deliver the
// same observable: terminated, never lost) and re-run post-ungil.
load("../harness.js", "caller relative");

const o = { k: 0 };
const ctl = { stop: 0, notifies: 0, parks: 0 };

// Notifier storm: dequeues waiters on (o, "k") as fast as possible so the
// carrier's W1 service window keeps racing a dequeue.
const notifier = new Thread(() => {
    let n = 0;
    while (Atomics.load(ctl, "stop") === 0) {
        Atomics.notify(o, "k");
        ++n;
        if ((n & 1023) === 0)
            Atomics.store(ctl, "notifies", n);
    }
    return n;
});

// Carrier park loop: re-park immediately after every wake. Short finite
// timeouts keep the carrier cycling park episodes (more W1 windows) while
// the storm keeps "ok" dequeues flowing. This loop must NOT exit on its
// own: only the watchdog termination ends it (by unwinding).
for (;;) {
    const r = Atomics.wait(o, "k", 0, 50);
    Atomics.add(ctl, "parks", 1);
    if (r !== "ok" && r !== "timed-out" && r !== "not-equal") {
        Atomics.store(ctl, "stop", 1);
        print("FAILURE: property Atomics.wait returned '" + r + "' under the notify/terminate race");
        throw new Error("MC-AINT S3: unexpected wait result " + r);
    }
}

// Unreachable: the for(;;) above never breaks; only termination unwinds it.
// (If a future edit adds a break, fail loudly rather than pass vacuously.)
Atomics.store(ctl, "stop", 1);
notifier.join();
print("FAILURE: carrier park loop exited normally under watchdog termination");
throw new Error("MC-AINT S3 violated: termination lost or never delivered");
