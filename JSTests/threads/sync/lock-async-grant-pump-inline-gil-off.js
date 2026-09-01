//@ requireOptions("--useJSThreads=1", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1")
// GIL-off: the async-grant pump for a SPAWNED head registrant must run inline
// on the releasing or notifying thread, never on vm.runLoop(). That run loop
// belongs to the main thread, which never services it while parked in join():
// the grant would wait on the pump, the registrant's keepalive on the grant,
// and join on the registrant.
//
// Also covers lock.asyncHold queued behind a spawned holder: the grant must
// still be delivered after the registrant finished (SPEC-api 4.6.2: tickets
// outlive their registering thread; an asyncHold does not keep the thread
// alive) while main is parked in join().
//
// The first case previously hung: the runner's timeout is the failure signal.
load("../harness.js", "caller relative");

// ---- cond.asyncWait re-grant: the waiter's thread body has returned and its
// drain loop is waiting on the counted registration; the notify runs on
// another spawned thread while main is parked in join(). ----
{
    const lock = new Lock();
    const cond = new Condition();
    const step = { n: 0 };
    const waiter = new Thread(() => {
        lock.hold(() => {
            cond.asyncWait(lock).then((release) => {
                Atomics.store(step, "n", 2);
                release();
            });
            Atomics.store(step, "n", 1);
            Atomics.notify(step, "n");
        });
    });
    Atomics.wait(step, "n", 0);
    const notifier = new Thread(() => {
        lock.hold(() => cond.notify());
    });
    notifier.join();
    waiter.join(); // Hung before the fix: the re-grant pump sat on main's run loop.
    shouldBe(Atomics.load(step, "n"), 2, "the re-granted asyncWait settled on the registrant before it completed");
}

// ---- lock.asyncHold queued behind a spawned holder: the registrant may
// finish with its ticket pending, and the grant after the holder's release
// must still deliver fn (on the registrant while it is alive, otherwise on
// the main fallback). ----
{
    const lock = new Lock();
    const step = { n: 0 };
    const result = { ran: 0 };
    const holder = new Thread(() => {
        lock.hold(() => {
            Atomics.store(step, "n", 1);
            Atomics.notify(step, "n");
            Atomics.wait(step, "n", 1); // hold until the asyncHold is queued
        });
    });
    Atomics.wait(step, "n", 0);
    let granted;
    const registrant = new Thread(() => {
        granted = lock.asyncHold(() => {
            Atomics.add(result, "ran", 1);
            return 7;
        });
        Atomics.store(step, "n", 2);
        Atomics.notify(step, "n");
    });
    registrant.join(); // Must not wait for the grant: the holder still holds the lock.
    holder.join();
    asyncTestStart(1);
    granted.then(v => {
        shouldBe(v, 7);
        shouldBe(Atomics.load(result, "ran"), 1, "the finished registrant's asyncHold fn ran exactly once");
        asyncTestPassed();
    });
}
