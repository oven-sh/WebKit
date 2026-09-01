//@ requireOptions("--useJSThreads=1")
// join() semantics: repeatability, joining already-finished threads,
// joining from other spawned threads, and concurrent joiners all agreeing.
load("../resources/assert.js", "caller relative");

// join() is repeatable and returns the identical value every time.
const obj = { tag: 1 };
const t1 = new Thread(() => obj);
shouldBe(t1.join(), obj);
shouldBe(t1.join(), obj);
shouldBe(t1.join(), obj);

// Joining a thread that has long since finished still works.
const t2 = new Thread(() => "done");
shouldBe(t2.join(), "done");
// Burn some time so t2 is thoroughly dead, then join again.
for (let i = 0; i < 1e6; ++i) { }
shouldBe(t2.join(), "done");

// A spawned thread can join another spawned thread.
const worker = new Thread(() => 1234);
const joiner = new Thread(w => w.join() + 1, worker);
shouldBe(joiner.join(), 1235);
shouldBe(worker.join(), 1234); // and the main thread can still join it too

// Multiple threads can all join the same thread; everyone sees the same
// result identity.
const target = new Thread(() => ({ unique: true }));
const observers = spawnN(4, () => target.join());
const seen = joinAll(observers);
const expected = target.join();
for (const value of seen)
    shouldBe(value, expected);

// A chain of joins: each thread waits for the previous one and accumulates.
let prev = new Thread(() => 0);
for (let i = 1; i <= 5; ++i)
    prev = new Thread((p, n) => p.join() + n, prev, i);
shouldBe(prev.join(), 15);

// join() returns after the thread body completed fully: side effects made
// by the thread are visible to the joiner (GIL or not, join is a
// synchronization edge).
const sideEffects = { log: [] };
const t3 = new Thread(se => {
    se.log.push("a");
    se.log.push("b");
    return se.log.length;
}, sideEffects);
shouldBe(t3.join(), 2);
shouldBe(sideEffects.log.length, 2);
shouldBe(sideEffects.log[0], "a");
shouldBe(sideEffects.log[1], "b");

// Joining many threads in reverse spawn order.
const batch = spawnN(6, i => i * i);
for (let i = 5; i >= 0; --i)
    shouldBe(batch[i].join(), i * i);

// join() takes no meaningful arguments; passing some is harmless.
shouldBe(new Thread(() => "args-ignored").join(1, 2, 3), "args-ignored");
