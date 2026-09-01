//@ requireOptions("--useJSThreads=1", "--watchdog=500", "--watchdog-exception-ok")
// Landed deviation D9 (docs/threads/INTEGRATE-api.md): contended
// Lock.prototype.hold acquisition parks in 10ms tryLockWithTimeout quanta
// and polls vm.hasTerminationRequest() between quanta — VMTraps cannot wake
// a thread blocked on the lock's native m_lock, so an unbounded park is
// unkillable under the watchdog when the holder can never release.
//
// Mechanics: lock.asyncHold() with no fn takes m_lock immediately at
// registration (5.5a A tryLock success — the grant is held by the ticket
// before the promise settles). The release function only arrives on a
// run-loop turn, and the main thread never yields to the run loop: it parks
// in lock.hold() below instead. So m_lock is held and can never be released
// — the contended hold would park forever. The watchdog fires at 500ms; the
// hold's quantum poll observes the termination request and throws the
// termination exception, which --watchdog-exception-ok maps to exit 0.
//
// Failure modes this catches:
// - hold acquires the unreleasable lock    => FAILURE print + plain Error
//   (nonzero exit even with --watchdog-exception-ok);
// - termination never observed (poll missing) => the run HANGS (the
//   runner/amplifier timeout reports it).
load("../harness.js", "caller relative");

const lock = new Lock();

lock.asyncHold(); // immediate grant: m_lock is held by the unsettled ticket
if (!lock.locked)
    throw new Error("setup: asyncHold's immediate grant should hold the lock at registration");

lock.hold(() => {
    // Unreachable: the grant's release fn is never delivered (no RL turn).
    print("FAILURE: contended hold acquired a lock whose holder can never release");
    throw new Error("D9 violated: hold acquired");
});

// Unreachable unless hold swallowed the termination:
print("FAILURE: hold returned normally under termination");
throw new Error("D9 violated: hold returned normally");
