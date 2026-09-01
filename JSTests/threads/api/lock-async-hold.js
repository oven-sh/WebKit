//@ requireOptions("--useJSThreads=1")
// API-I12: 5.5 promises settle on a run-loop turn, never synchronously
//          inside the registering call.
// API-I23: asyncHold(fn) whose fn calls cond.asyncWait(lock): no Error, no
//          double-unlock, settles with fn's result; later acquirers proceed
//          (5.5a E consumed-ticket path).
// Release contract (4.2): no-fn asyncHold resolves with a release function
//          to call exactly once — second call => Error.
// Barging (4.2): sync-vs-async order is unspecified; a sync hold may
//          overtake a queued async ticket, and the ticket must still be
//          granted afterwards.
load("../harness.js", "caller relative");

asyncTestStart(8);

const lock = new Lock();
const cond = new Condition();

(async () => {
    // ---- 1. no-fn arity: release contract + I12 ----
    {
        let settled = false;
        const p = lock.asyncHold();
        shouldBeTrue(p instanceof Promise);
        p.then(() => { settled = true; });
        shouldBeFalse(settled, "I12: asyncHold must not settle synchronously");
        // The GRANT itself may happen at registration (5.5a A tryLock
        // success): the lock reads held even before the promise settles.
        shouldBeTrue(lock.locked);

        const release = await p;
        shouldBe(typeof release, "function");
        shouldBeTrue(lock.locked);
        shouldBe(release(), undefined);
        shouldBeFalse(lock.locked);
        shouldThrow(Error, () => release(), "Lock release function called more than once");
        shouldBeFalse(lock.locked, "double release must not unlock anything");
        asyncTestPassed();
    }

    // ---- 2. with-fn arity: fn runs holding the lock on a RL turn; the
    // promise settles with fn's result after the implicit release (E). ----
    {
        let fnRan = false;
        const p = lock.asyncHold(() => {
            fnRan = true;
            shouldBeTrue(lock.locked, "fn must run holding the lock");
            return "fn-result";
        });
        shouldBeFalse(fnRan, "I12: fn runs on a run-loop turn, not synchronously");
        shouldBe(await p, "fn-result");
        shouldBeTrue(fnRan);
        shouldBeFalse(lock.locked, "implicit release (E) after fn");
        asyncTestPassed();
    }

    // ---- 3. with-fn arity: fn throw => rejection, lock still released ----
    {
        const boom = new Error("boom");
        let rejectedWith = null;
        await lock.asyncHold(() => { throw boom; }).then(
            () => { throw new Error("must reject"); },
            e => { rejectedWith = e; });
        shouldBe(rejectedWith, boom);
        shouldBeFalse(lock.locked);
        asyncTestPassed();
    }

    // ---- 4. async tickets are FIFO (4.2) ----
    {
        const order = [];
        const pa = lock.asyncHold(() => { order.push("a"); });
        const pb = lock.asyncHold(() => { order.push("b"); });
        const pc = lock.asyncHold(() => { order.push("c"); });
        await Promise.all([pa, pb, pc]);
        shouldBe(order.join(","), "a,b,c", "async tickets grant in FIFO order");
        shouldBeFalse(lock.locked);
        asyncTestPassed();
    }

    // ---- 5. barging: a sync hold taken before the pump's RL turn overtakes
    // a queued async ticket; legal (order unspecified), and the ticket must
    // still be granted afterwards. ----
    {
        let ticket;
        const t = new Thread(() => lock.asyncHold()); // registered on the spawned thread
        lock.hold(() => {
            ticket = t.join(); // thread queues its ticket against our hold
            shouldBeTrue(ticket instanceof Promise);
        });
        // The release scheduled a pump on the run loop, but no RL turn has
        // happened yet: this sync hold barges in via tryLock.
        let barged = false;
        lock.hold(() => { barged = true; });
        shouldBeTrue(barged, "sync hold may barge ahead of a queued async ticket");
        const release = await ticket; // the barged-past ticket still gets the lock
        shouldBeTrue(lock.locked);
        release();
        shouldBeFalse(lock.locked);
        asyncTestPassed();
    }

    // ---- 6. async-held is not recursive for sync hold or a second
    // registrant — callers queue / Error per 4.2 ----
    {
        const release = await lock.asyncHold();
        // sync-holding caller check is m_holder-based: we do NOT sync-hold,
        // so asyncHold from here is legal and simply queues.
        const queued = lock.asyncHold(() => "queued-ran");
        let queuedSettled = false;
        queued.then(() => { queuedSettled = true; });
        await Promise.resolve(); // give microtasks a chance: still held
        shouldBeFalse(queuedSettled, "second ticket must wait for release");
        release();
        shouldBe(await queued, "queued-ran");
        asyncTestPassed();
    }

    // ---- 7. I23: asyncHold(fn) + cond.asyncWait(lock) inside fn ----
    {
        let waitPromise;
        const p = lock.asyncHold(() => {
            waitPromise = cond.asyncWait(lock); // consumes the hold (4.3(b))
            shouldBeFalse(lock.locked, "asyncWait releases the lock immediately");
            return 23;
        });
        // E's CAS loses to the asyncWait consumption: no Error, no
        // double-unlock, the promise still settles with fn's result.
        shouldBe(await p, 23);
        // Later acquirers proceed (the lock is free, not wedged):
        await lock.asyncHold(() => {
            shouldBe(cond.notify(), 1); // wake the asyncWait ticket
        });
        // After notify the wait ticket re-queues via the 5.5a A-failure path
        // and is granted once the lock frees; it resolves with a fresh
        // release function (no-fn contract).
        const release = await waitPromise;
        shouldBe(typeof release, "function");
        shouldBeTrue(lock.locked);
        release();
        shouldBeFalse(lock.locked);
        // The lock remains generally usable.
        shouldBe(lock.hold(() => "after-I23"), "after-I23");
        asyncTestPassed();
    }

    // ---- 8. D10 (docs/threads/INTEGRATE-api.md): sync hold / sync
    // cond.wait inside an asyncHold-delivered fn. The delivered fn runs
    // with the lock async-held (invisible to the sync m_holder): a sync
    // lock.hold(g) on the SAME lock from inside fn can never succeed — its
    // only release point is this fn's own post-fn epilogue — so it must
    // throw "Lock is not recursive" (the m_asyncGrantRunner guard), never
    // park. Sync cond.wait requires a 5.3 sync hold per the frozen 4.3, so
    // it throws TypeError (use cond.asyncWait, 4.3(b)). ----
    {
        const p = lock.asyncHold(() => {
            shouldThrow(Error, () => lock.hold(() => {
                throw new Error("unreachable: sync hold inside asyncHold fn acquired the lock");
            }), "Lock is not recursive");
            shouldBeTrue(lock.locked, "guarded throw must not disturb the live grant");
            shouldThrow(TypeError, () => cond.wait(lock));
            shouldBeTrue(lock.locked, "sync cond.wait TypeError must not consume the async hold");
            return "guarded";
        });
        shouldBe(await p, "guarded");
        shouldBeFalse(lock.locked, "implicit release (E) still ran after the guarded throws");
        // After consumption by asyncWait (4.3(b)) the grant is dead and a
        // sync hold from the rest of fn is legal again (runner cleared by
        // the release path):
        let waitPromise2;
        const p2 = lock.asyncHold(() => {
            waitPromise2 = cond.asyncWait(lock); // consumes the hold; lock now free
            shouldBeFalse(lock.locked);
            shouldBe(lock.hold(() => "post-consumption-hold"), "post-consumption-hold");
            return "post-consumption";
        });
        shouldBe(await p2, "post-consumption");
        shouldBeFalse(lock.locked);
        // Settle the wait ticket so it does not pin the shell (4.6.3):
        shouldBe(cond.notify(), 1);
        const release2 = await waitPromise2;
        release2();
        shouldBeFalse(lock.locked);
        asyncTestPassed();
    }
})();
