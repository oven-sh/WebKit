//@ requireOptions("--useJSThreads=1")
// API-I1: the typed-array Atomics path keeps ECMA-262 ValidateAtomicAccess
// ordering under --useJSThreads. The length is snapshotted BEFORE ToIndex, so
// an index whose valueOf detaches the buffer and then returns an
// out-of-bounds value must still throw RangeError (step 4), exactly as the
// flag-off engine does; only an in-bounds index reaches the detached
// TypeError (RevalidateAtomicAccess). The threads-mode concurrent-detach
// re-check must never turn the sequential RangeError into a TypeError.
load("../resources/assert.js", "caller relative");

const oobMessage = "RangeError: Access index out of bounds for atomic access.";

function detachingIndex(buffer, index) {
    return { valueOf() { buffer.transfer(); return index; } };
}

// ---- Fixed-length views: every RMW entry point, load, store, notify ----
{
    const ops = {
        load: (ta, index) => Atomics.load(ta, index),
        store: (ta, index) => Atomics.store(ta, index, 1),
        add: (ta, index) => Atomics.add(ta, index, 1),
        sub: (ta, index) => Atomics.sub(ta, index, 1),
        and: (ta, index) => Atomics.and(ta, index, 1),
        or: (ta, index) => Atomics.or(ta, index, 1),
        xor: (ta, index) => Atomics.xor(ta, index, 1),
        exchange: (ta, index) => Atomics.exchange(ta, index, 1),
        compareExchange: (ta, index) => Atomics.compareExchange(ta, index, 0, 1),
        notify: (ta, index) => Atomics.notify(ta, index, 1),
    };
    for (const [name, op] of Object.entries(ops)) {
        // Detach inside ToIndex, out-of-bounds result: RangeError.
        {
            const ta = new Int32Array(10);
            shouldThrow(RangeError, () => op(ta, detachingIndex(ta.buffer, 100)), oobMessage);
            shouldBeTrue(ta.buffer.detached, name + ": buffer detached");
        }
        // Detach inside ToIndex, in-bounds result: detached TypeError
        // (notify on a now-detached non-shared buffer returns 0 instead).
        {
            const ta = new Int32Array(10);
            if (name === "notify")
                shouldBe(op(ta, detachingIndex(ta.buffer, 5)), 0, name);
            else
                shouldThrow(TypeError, () => op(ta, detachingIndex(ta.buffer, 5)));
        }
        // Already detached before the call: TypeError regardless of index.
        {
            const ta = new Int32Array(10);
            ta.buffer.transfer();
            shouldThrow(TypeError, () => op(ta, 0));
            shouldThrow(TypeError, () => op(ta, 100));
        }
        // Plain out-of-bounds on a live buffer: RangeError.
        {
            const ta = new Int32Array(10);
            shouldThrow(RangeError, () => op(ta, 10), oobMessage);
            shouldThrow(RangeError, () => op(ta, 100), oobMessage);
        }
    }
}

// ---- Length-tracking view over a resizable buffer ----
{
    const buffer = new ArrayBuffer(40, { maxByteLength: 80 });
    const ta = new Int32Array(buffer);
    shouldBe(ta.length, 10);
    shouldThrow(RangeError, () => Atomics.load(ta, detachingIndex(buffer, 100)), oobMessage);
    shouldBeTrue(buffer.detached);
    shouldThrow(TypeError, () => Atomics.load(ta, 0));
}
{
    const buffer = new ArrayBuffer(40, { maxByteLength: 80 });
    const ta = new Int32Array(buffer);
    shouldThrow(TypeError, () => Atomics.add(ta, detachingIndex(buffer, 5), 1));
}

// ---- BigInt64 views take the same path ----
{
    const ta = new BigInt64Array(4);
    shouldThrow(RangeError, () => Atomics.load(ta, detachingIndex(ta.buffer, 4)), oobMessage);
}
{
    const ta = new BigInt64Array(4);
    shouldThrow(TypeError, () => Atomics.store(ta, detachingIndex(ta.buffer, 1), 1n));
}

// ---- Zero-length view: index 0 is out of bounds, detach or not ----
{
    const ta = new Int32Array(0);
    shouldThrow(RangeError, () => Atomics.load(ta, detachingIndex(ta.buffer, 0)), oobMessage);
}
