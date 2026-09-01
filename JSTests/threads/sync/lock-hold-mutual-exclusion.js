//@ requireOptions("--useJSThreads=1")
// Lock.prototype.hold provides mutual exclusion across threads.
//
// Stub-scheduling note (phase-1 GIL): worker threads must be self-sufficient.
// Main only spawns and joins; all cross-thread waking happens between workers
// (a worker parked in a blocking op must never depend on the main thread,
// which may itself be parked in join, to wake it).
load("../resources/assert.js", "caller relative");

const THREADS = 4;
const ITERATIONS = 250;

const lock = new Lock();
const shared = {
    counter: 0,        // incremented non-atomically under the lock
    inside: 0,         // exclusion canary: must always be 0 on entry
    log: "",           // multi-step mutation under the lock
    maxInside: 0,
    violations: 0,
};

const workers = spawnN(THREADS, index => {
    for (let i = 0; i < ITERATIONS; ++i) {
        lock.hold(() => {
            // Exclusion canary: any overlap of two critical sections shows up
            // as inside !== 0 here or maxInside > 1 below.
            if (shared.inside !== 0)
                shared.violations++;
            shared.inside++;
            if (shared.inside > shared.maxInside)
                shared.maxInside = shared.inside;

            // Deliberately non-atomic read-modify-write sequences. Only the
            // lock makes these safe.
            const before = shared.counter;
            shared.counter = before + 1;

            // Multi-step structural mutation: append a record then truncate,
            // so a racing observer inside the critical section would see a
            // torn intermediate state.
            shared.log += index;
            if (shared.log.length > 8)
                shared.log = shared.log.slice(-8);

            shared.inside--;
        });
    }
    return index;
});

const results = joinAll(workers);
for (let i = 0; i < THREADS; ++i)
    shouldBe(results[i], i, "worker " + i + " result");

shouldBe(shared.counter, THREADS * ITERATIONS, "all increments retained");
shouldBe(shared.violations, 0, "no critical-section overlap observed");
shouldBe(shared.maxInside, 1, "at most one thread inside at a time");
shouldBe(shared.inside, 0, "balanced enter/exit");
shouldBeTrue(shared.log.length <= 8, "log invariant maintained");
shouldBeFalse(lock.locked, "lock released after all workers finish");

// The same lock still works on main afterwards.
shouldBe(lock.hold(() => ++shared.counter), THREADS * ITERATIONS + 1);

// Exceptions inside a worker's critical section release the lock for other
// threads (each worker throws once mid-loop and keeps going).
const lock2 = new Lock();
const shared2 = { counter: 0, inside: 0, violations: 0 };
const throwers = spawnN(THREADS, () => {
    let caught = 0;
    for (let i = 0; i < 50; ++i) {
        try {
            lock2.hold(() => {
                if (shared2.inside !== 0)
                    shared2.violations++;
                shared2.inside++;
                shared2.counter++;
                shared2.inside--;
                if (i === 25)
                    throw new Error("released?");
            });
        } catch (e) {
            caught++;
        }
    }
    return caught;
});
const caughtCounts = joinAll(throwers);
for (const caught of caughtCounts)
    shouldBe(caught, 1, "each worker observed exactly its own throw");
shouldBe(shared2.counter, THREADS * 50);
shouldBe(shared2.violations, 0);
shouldBeFalse(lock2.locked);
