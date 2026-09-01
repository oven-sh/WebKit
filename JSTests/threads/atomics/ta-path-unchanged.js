//@ runDefault
//@ runDefault("--useJSThreads=1")
// API-I1: the typed-array Atomics path is unchanged by --useJSThreads.
//
// SPEC-api 4.5 steps 0-3: with the flag off, today's body runs textually
// intact (the property-dispatch steps do not exist); with the flag on, any
// arg0 that is a JSArrayBufferView (including float-typed views) or a
// non-object takes today's path with identical results AND identical errors
// (the sole carve-out, 4.5-1a / I21, applies only on spawned Threads and is
// covered by atomics/ta-wait-thread-gate.js, not here). This file therefore
// runs BOTH ways (annex T2) and every assertion must hold identically in
// both runs. Main thread only; no Thread is spawned.
//
// API-I19 (flag-off PERF identity) is the bench-side counterpart of this
// invariant: it is gated by Tools/threads/bench-gate.sh --record/gate
// against the integrator-recorded pre-workstream baseline (G15; SPEC-api §6
// I19 — an INT gate, not assertable from JS), so this file carries the
// corpus citation while the bench gate carries the measurement.
load("../resources/assert.js", "caller relative");

// ---- Int32Array over a plain ArrayBuffer: RMW family + load/store ----
{
    const i32 = new Int32Array(new ArrayBuffer(16));

    shouldBe(Atomics.store(i32, 0, 5), 5);
    shouldBe(i32[0], 5);
    shouldBe(Atomics.load(i32, 0), 5);

    shouldBe(Atomics.add(i32, 0, 3), 5); // returns old value
    shouldBe(i32[0], 8);
    shouldBe(Atomics.sub(i32, 0, 2), 8);
    shouldBe(i32[0], 6);
    shouldBe(Atomics.and(i32, 0, 3), 6);
    shouldBe(i32[0], 2);
    shouldBe(Atomics.or(i32, 0, 5), 2);
    shouldBe(i32[0], 7);
    shouldBe(Atomics.xor(i32, 0, 1), 7);
    shouldBe(i32[0], 6);
    shouldBe(Atomics.exchange(i32, 0, 42), 6);
    shouldBe(i32[0], 42);

    // compareExchange: returns the value read either way.
    shouldBe(Atomics.compareExchange(i32, 0, 42, 100), 42);
    shouldBe(i32[0], 100);
    shouldBe(Atomics.compareExchange(i32, 0, 999, 0), 100); // mismatch: no store
    shouldBe(i32[0], 100);

    // Index coercion (toIndex): string indices work.
    shouldBe(Atomics.store(i32, "1", 11), 11);
    shouldBe(Atomics.load(i32, "1"), 11);

    // Value coercion order and side effects are today's: operand valueOf runs.
    let effects = "";
    shouldBe(Atomics.store(i32, 2, { valueOf() { effects += "v"; return 9; } }), 9);
    shouldBe(effects, "v");
    shouldBe(i32[2], 9);

    // Atomics.store returns ToIntegerOrInfinity(v), not the truncated lane value.
    shouldBe(Atomics.store(i32, 3, 7.9), 7);
    shouldBe(i32[3], 7);

    // notify on a non-shared view: no waiters possible, returns 0.
    shouldBe(Atomics.notify(i32, 0), 0);
    shouldBe(Atomics.notify(i32, 0, 1), 0);

    // wait on a non-shared view: TypeError, exact message.
    shouldThrow(TypeError, () => Atomics.wait(i32, 0, 0),
        "TypeError: Typed array for wait/waitAsync/notify must wrap a SharedArrayBuffer.");
    shouldThrow(TypeError, () => Atomics.waitAsync(i32, 0, 0),
        "TypeError: Typed array for wait/waitAsync/notify must wrap a SharedArrayBuffer.");
}

// ---- Other integer view types stay on the typed-array path ----
{
    const u8 = new Uint8Array(8);
    shouldBe(Atomics.add(u8, 0, 200), 0);
    shouldBe(Atomics.add(u8, 0, 200), 200);
    shouldBe(u8[0], (400 & 0xff));
    shouldBe(Atomics.exchange(u8, 0, 0), 400 & 0xff);

    const u16 = new Uint16Array(4);
    shouldBe(Atomics.store(u16, 0, 0x12345), 0x12345); // returns ToIntegerOrInfinity
    shouldBe(u16[0], 0x12345 & 0xffff);

    const u32 = new Uint32Array(4);
    shouldBe(Atomics.store(u32, 0, -1), -1);
    shouldBe(u32[0], 0xffffffff);
    shouldBe(Atomics.load(u32, 0), 0xffffffff);
}

// ---- BigInt64Array: BigInt operands required, BigInt results ----
{
    const b64 = new BigInt64Array(4);
    shouldBe(Atomics.store(b64, 0, 5n), 5n);
    shouldBe(Atomics.add(b64, 0, 3n), 5n);
    shouldBe(Atomics.load(b64, 0), 8n);
    shouldBe(Atomics.compareExchange(b64, 0, 8n, -1n), 8n);
    shouldBe(b64[0], -1n);

    // Mixing Number and BigInt lanes throws TypeError, exactly as today.
    shouldThrow(TypeError, () => Atomics.add(b64, 0, 1));
    shouldThrow(TypeError, () => Atomics.store(b64, 0, 1));
    const i32 = new Int32Array(4);
    shouldThrow(TypeError, () => Atomics.add(i32, 0, 1n));
}

// ---- SharedArrayBuffer-backed views: wait/waitAsync/notify fast paths ----
if (typeof SharedArrayBuffer === "function") {
    const si32 = new Int32Array(new SharedArrayBuffer(16));

    shouldBe(Atomics.store(si32, 0, 0), 0);
    // Value mismatch: returns without blocking.
    shouldBe(Atomics.wait(si32, 0, 1), "not-equal");
    // Value match, zero timeout: returns without blocking.
    shouldBe(Atomics.wait(si32, 0, 0, 0), "timed-out");

    const notEqual = Atomics.waitAsync(si32, 0, 1);
    shouldBe(notEqual.async, false);
    shouldBe(notEqual.value, "not-equal");
    const timedOut = Atomics.waitAsync(si32, 0, 0, 0);
    shouldBe(timedOut.async, false);
    shouldBe(timedOut.value, "timed-out");

    // No waiters: notify returns 0.
    shouldBe(Atomics.notify(si32, 0), 0);
    shouldBe(Atomics.notify(si32, 0, 0), 0);

    // wait requires Int32Array or BigInt64Array even when shared.
    const su32 = new Uint32Array(new SharedArrayBuffer(16));
    shouldThrow(TypeError, () => Atomics.wait(su32, 0, 0),
        "TypeError: Typed array argument must be an Int32Array or BigInt64Array.");
    shouldThrow(TypeError, () => Atomics.waitAsync(su32, 0, 0),
        "TypeError: Typed array argument must be an Int32Array or BigInt64Array.");
    shouldThrow(TypeError, () => Atomics.notify(su32, 0),
        "TypeError: Typed array argument must be an Int32Array or BigInt64Array.");

    // RMW family works on shared views too.
    shouldBe(Atomics.add(si32, 1, 7), 0);
    shouldBe(Atomics.load(si32, 1), 7);
}

// ---- Errors: float-typed and non-integer views stay rejected (step 1: any
// view keeps today's path, so these are TypeErrors with today's message) ----
{
    const f64 = new Float64Array(4);
    const f32 = new Float32Array(4);
    const c8 = new Uint8ClampedArray(4);
    const dv = new DataView(new ArrayBuffer(8));
    const integerMessage = "TypeError: Typed array argument must be an Int8Array, Int16Array, Int32Array, Uint8Array, Uint16Array, Uint32Array, BigInt64Array, or BigUint64Array.";
    for (const view of [f64, f32, c8]) {
        shouldThrow(TypeError, () => Atomics.load(view, 0), integerMessage);
        shouldThrow(TypeError, () => Atomics.store(view, 0, 0), integerMessage);
        shouldThrow(TypeError, () => Atomics.add(view, 0, 1), integerMessage);
        shouldThrow(TypeError, () => Atomics.compareExchange(view, 0, 0, 1), integerMessage);
        shouldThrow(TypeError, () => Atomics.wait(view, 0, 0));
        shouldThrow(TypeError, () => Atomics.notify(view, 0));
    }
    shouldThrow(TypeError, () => Atomics.load(dv, 0));
    shouldThrow(TypeError, () => Atomics.store(dv, 0, 0));
}

// ---- Errors: non-object arg0 takes step 3, "as today" (never the property
// path, never the 1a gate) ----
{
    for (const notAnObject of [undefined, null, 42, "abc", true, Symbol("s"), 7n]) {
        shouldThrow(TypeError, () => Atomics.load(notAnObject, 0));
        shouldThrow(TypeError, () => Atomics.store(notAnObject, 0, 0));
        shouldThrow(TypeError, () => Atomics.add(notAnObject, 0, 1));
        shouldThrow(TypeError, () => Atomics.exchange(notAnObject, 0, 1));
        shouldThrow(TypeError, () => Atomics.compareExchange(notAnObject, 0, 0, 1));
        shouldThrow(TypeError, () => Atomics.wait(notAnObject, 0, 0));
        shouldThrow(TypeError, () => Atomics.waitAsync(notAnObject, 0, 0));
        shouldThrow(TypeError, () => Atomics.notify(notAnObject, 0));
    }
}

// ---- Errors: out-of-bounds / bad indices on views (today's RangeErrors) ----
{
    const i32 = new Int32Array(4);
    const oobMessage = "RangeError: Access index out of bounds for atomic access.";
    shouldThrow(RangeError, () => Atomics.load(i32, 4), oobMessage);
    shouldThrow(RangeError, () => Atomics.store(i32, 100, 0), oobMessage);
    shouldThrow(RangeError, () => Atomics.add(i32, 4, 1), oobMessage);
    shouldThrow(RangeError, () => Atomics.load(i32, -1));
    // ToIndex truncates fractional indices (ES ValidateAtomicAccess): 1.5 -> 1,
    // no throw. Verify it really lands on index 1, today's path both runs.
    shouldBe(Atomics.store(i32, 1.5, 7), 7);
    shouldBe(i32[1], 7);
    shouldBe(Atomics.load(i32, 1.5), 7);
}

// ---- Detached buffers keep today's behavior (guarded: shell helper) ----
if (typeof transferArrayBuffer === "function") {
    const buffer = new ArrayBuffer(16);
    const i32 = new Int32Array(buffer);
    transferArrayBuffer(buffer);
    shouldThrow(TypeError, () => Atomics.load(i32, 0));
    shouldThrow(TypeError, () => Atomics.store(i32, 0, 1));
    shouldThrow(TypeError, () => Atomics.add(i32, 0, 1));
}

// ---- isLockFree / pause are untouched by the dispatch split ----
{
    shouldBe(Atomics.isLockFree(1), true);
    shouldBe(Atomics.isLockFree(2), true);
    shouldBe(Atomics.isLockFree(4), true);
    shouldBe(Atomics.isLockFree(8), true);
    shouldBe(Atomics.isLockFree(3), false);
    shouldBe(Atomics.isLockFree(0), false);
    shouldBe(Atomics.isLockFree(16), false);

    shouldBe(Atomics.pause(), undefined);
    shouldBe(Atomics.pause(1), undefined);
    shouldBe(Atomics.pause(undefined), undefined);
    shouldThrow(TypeError, () => Atomics.pause(0.5));
    shouldThrow(TypeError, () => Atomics.pause("x"));
}

// ---- Surface shape: function lengths and names are today's ----
{
    shouldBe(Atomics.add.length, 3);
    shouldBe(Atomics.and.length, 3);
    shouldBe(Atomics.compareExchange.length, 4);
    shouldBe(Atomics.exchange.length, 3);
    shouldBe(Atomics.isLockFree.length, 1);
    shouldBe(Atomics.load.length, 2);
    shouldBe(Atomics.notify.length, 3);
    shouldBe(Atomics.or.length, 3);
    shouldBe(Atomics.store.length, 3);
    shouldBe(Atomics.sub.length, 3);
    shouldBe(Atomics.wait.length, 4);
    shouldBe(Atomics.xor.length, 3);
    shouldBe(Atomics[Symbol.toStringTag], "Atomics");
}
