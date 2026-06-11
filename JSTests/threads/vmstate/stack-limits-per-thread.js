//@ requireOptions("--useJSThreads=1")
// SPEC-vmstate W3 Group 3 / §6.1.3: per-thread stack limits under the GIL
// come from the JSLock hand-off (didAcquireLock rewrites lastStackTop /
// stackPointerAtVMEntry; VMTraps::m_stack carries the limits generated code
// checks). Every thread — each on its own native stack — must get a clean
// RangeError from unbounded recursion, recover, and keep executing; a stale
// limit from another thread's stack would either crash or throw absurdly
// early/late.
load("../resources/assert.js", "caller relative");

const THREADS = 4;

function overflowDepth() {
    let depth = 0;
    function deep() {
        ++depth;
        deep();
    }
    try {
        deep();
    } catch (e) {
        if (!(e instanceof RangeError))
            throw new Error("expected RangeError, got " + e);
        return depth;
    }
    throw new Error("recursion never hit the stack limit");
}

// Main thread overflows and recovers.
const mainDepth = overflowDepth();
shouldBeTrue(mainDepth > 100, "main thread overflow depth sane");

const threads = spawnN(THREADS, t => {
    // Overflow twice per thread: the limit must be re-armed correctly after
    // the first unwind (catch fully rolls back the frame bookkeeping).
    const first = overflowDepth();
    const second = overflowDepth();
    if (first <= 100 || second <= 100)
        throw new Error("thread " + t + " overflowed implausibly early: " + first + "/" + second);
    // After recovery the thread still does real work at depth.
    function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
    return fib(15);
});

for (const value of joinAll(threads))
    shouldBe(value, 610);

// Main thread again after the spawned threads' hand-offs: its own limits
// must have been restored by the lock hand-off.
shouldBeTrue(overflowDepth() > 100, "main thread overflow after threads");
