//@ requireOptions("--useJSThreads=1")
// Thread.current and Thread#id: identity, stability, uniqueness, and
// self-join rejection.
load("../resources/assert.js", "caller relative");

// Main thread: tid 0, stable identity.
const main = Thread.current;
shouldBeTrue(main instanceof Thread);
shouldBe(main.id, 0);
shouldBe(Thread.current, main); // same cell every time
shouldBe(Thread.current.id, 0);
shouldBe(typeof main.id, "number");

// The main thread cannot join itself.
shouldThrow(() => main.join(), "Error: Thread cannot join itself");
shouldThrow(() => Thread.current.join());

// Spawned threads get non-zero, distinct ids; t.id agrees with what the
// thread sees via Thread.current.id.
const t1 = new Thread(() => Thread.current.id);
const innerId = t1.join();
shouldBe(innerId, t1.id);
shouldBeTrue(t1.id !== 0);

// Inside a spawned thread, Thread.current is the same cell as the Thread
// object the constructor returned.
const witness = {};
const t2 = new Thread(w => {
    w.current = Thread.current;
    w.stable = Thread.current === Thread.current;
    return Thread.current.id;
}, witness);
const t2Id = t2.join();
shouldBe(witness.current, t2);
shouldBeTrue(witness.stable);
shouldBe(t2Id, t2.id);

// A spawned thread cannot join itself, whether through Thread.current or
// through its own Thread object handed in from outside.
const selfJoin = new Thread(() => {
    let viaCurrent = false;
    try {
        Thread.current.join();
    } catch {
        viaCurrent = true;
    }
    return viaCurrent;
});
shouldBeTrue(selfJoin.join());

// Self-join through the thread's own Thread object handed in from outside.
// The main thread keeps the GIL until t3.join() drops it, so the
// `holder.self = t3` store below is guaranteed to happen before the thread
// body runs.
const holder = {};
const t3 = new Thread(h => {
    try {
        h.self.join();
        return "no-throw";
    } catch (e) {
        return "threw:" + (e instanceof Error);
    }
}, holder);
holder.self = t3;
shouldBe(t3.join(), "threw:true");

// Joining the *parent* thread (via its Thread.current handle) is legal: the
// parent finishes immediately after spawning the inner thread, so the
// inner's join of the parent succeeds and observes the parent's result.
const parent = new Thread(() => {
    const inner = new Thread(parentHandle => {
        parentHandle.join(); // parent finished (or will finish); not a self-join
        return "joined-parent";
    }, Thread.current);
    return inner;
});
const inner = parent.join();
shouldBeTrue(inner instanceof Thread);
shouldBe(inner.join(), "joined-parent");

// Ids are unique among live threads.
const batch = spawnN(6, () => Thread.current.id);
const ids = joinAll(batch);
const set = new Set(ids);
shouldBe(set.size, 6);
shouldBeFalse(set.has(0));
for (let i = 0; i < 6; ++i)
    shouldBe(ids[i], batch[i].id);

// id is an accessor on the prototype, not an own property of instances.
shouldBeFalse(Object.getOwnPropertyNames(t1).includes("id"));
shouldThrow(TypeError, () => Object.getOwnPropertyDescriptor(Thread.prototype, "id").get.call({}));

// id keeps answering after the thread has finished.
shouldBe(t1.id, innerId);
