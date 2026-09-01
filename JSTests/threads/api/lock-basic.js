//@ requireOptions("--useJSThreads=1")
// API-I6 (small-N, main thread contending): N x M lock.hold(() => counter++)
//        on a shared property yields exactly N*M increments.
// API-I7: hold(fn) releases on throw; a later hold from another thread
//        succeeds.
// API-I8: nested same-thread hold (incl. main) throws Error, no deadlock,
//        and the outer hold is kept.
load("../harness.js", "caller relative");

const lock = new Lock();

// ---- basics: hold returns fn's value; locked getter flips (tests-only,
// racy by spec — but single-threaded here, so exact) ----
shouldBeFalse(lock.locked);
shouldBe(lock.hold(() => {
    shouldBeTrue(lock.locked);
    return 42;
}), 42);
shouldBeFalse(lock.locked);

// uncontended hold never blocks and is always allowed (4.2 tryLock first)
shouldBe(lock.hold(() => "uncontended"), "uncontended");

// ---- I8: non-recursive, main thread ----
lock.hold(() => {
    const e = shouldThrow(Error, () => lock.hold(() => {
        throw new Error("inner fn must not run");
    }), "Lock is not recursive");
    shouldBe(e.constructor, Error);
    shouldBeTrue(lock.locked, "outer hold must survive the inner Error");
});
shouldBeFalse(lock.locked, "outer hold released normally after the inner Error");

// ---- I8 on a spawned thread ----
shouldBe(new Thread(() => lock.hold(() => {
    try {
        lock.hold(() => 0);
        return "inner hold did not throw";
    } catch (e) {
        return (e instanceof Error) ? e.message : "wrong exception kind";
    }
})).join(), "Lock is not recursive");

// ---- I7: throw releases; later holds (same and other thread) succeed ----
{
    const boom = new Error("boom");
    shouldBe(shouldThrow(Error, () => lock.hold(() => { throw boom; })), boom);
    shouldBeFalse(lock.locked, "throwing hold must release");
    shouldBe(lock.hold(() => "same-thread-after-throw"), "same-thread-after-throw");
    shouldBe(new Thread(() => lock.hold(() => "other-thread-after-throw")).join(),
        "other-thread-after-throw");
}

// ---- I6 small-N: N spawned threads x M holds + M holds from main, all
// incrementing a shared property under the lock. The main thread contends:
// its holds and the threads' holds interleave at the holds' own yield
// points (contended hold drops the GIL, 5.2) — no preemption assumed. ----
{
    const N = 3;
    const M = 2000;
    const counter = { n: 0 };
    const threads = spawnN(N, () => {
        for (let i = 0; i < M; ++i)
            lock.hold(() => { counter.n++; });
    });
    // Main contends with its own M holds; each release pumps/wakes (5.3).
    for (let i = 0; i < M; ++i)
        lock.hold(() => { counter.n++; });
    joinAll(threads);
    shouldBe(counter.n, (N + 1) * M, "no lost increments under lock.hold");
    shouldBeFalse(lock.locked);
}

// ---- mutual exclusion is real: a thread parked on hold() observes the
// protected invariant only after the holder restores it ----
{
    const box = { a: 0, b: 0 };
    const t = new Thread(() => lock.hold(() => box.a === box.b));
    const ok = lock.hold(() => {
        box.a = 1; // invariant a===b broken while held
        // t cannot enter here: it parks on m_lock until we release.
        box.b = 1; // restored before release
        return true;
    });
    shouldBeTrue(ok);
    shouldBeTrue(t.join(), "waiter must never see the broken invariant");
}
