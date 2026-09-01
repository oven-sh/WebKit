//@ requireOptions("--useJSThreads=1")
// API-I14: Thread.restrict + ConcurrentAccessError (SPEC-api 4.1, 5.7, Dev 8/11).
//
// SKIPPED until the 9.2-6 choke-point hook is INTEGRATOR-applied (I14: "INT
// gate via 9.2-6; //@ skipped until then"). The exclusion/idempotency/owner
// halves would pass without the hook, but the foreign-thread CAE half cannot,
// so the whole file stays skipped to keep CI green until integration; the
// integrator deletes the `//@ skip` line when applying the 9.2-6 diff.
//
// Covered (I14):
// - every Dev-8 enforced op from a thread != T throws ConcurrentAccessError:
//   full named set (get, set, has, delete, defineProperty, ownKeys,
//   setPrototypeOf, isExtensible, preventExtensions), indexed
//   set/delete/define on an array, indexed set on a plain {} after the owner
//   adds o[0];
// - T (the owner) unaffected; values unchanged; survives 5.7.1 warm-ups
//   (IC-warmed loops before AND after restrict);
// - owner double-restrict returns o (5.7.1-0); re-restrict from another
//   thread throws CAE;
// - post-bad-time (SlowPut) array restricts OK;
// - Dev-8/11 excluded receivers throw TypeError "cannot restrict this object";
// - D13 (round 4) method-table-overrider INSTANCES (typed arrays, DataView,
//   String objects, arguments objects, functions, RegExp instances) throw
//   the same TypeError at restrict time — they cannot be enforced by the
//   9.2-6 hooks, so they are never accepted;
// - Dev-8 UNenforced set (getPrototypeOf, call/construct, indexed GET) is
//   deliberately untested per I14.
//
// Conventions (annex T2): self-checking, failure = throw; every spawned
// thread is joined; no preemptive-GIL reliance (every spawned fn runs to
// completion without needing to be preempted); blocking ops bounded (join on
// threads whose fn terminates unconditionally).
load("../resources/assert.js", "caller relative");

const WARM = 2e3;

function warmUp(o) {
    // 5.7.1 warm-ups: get + put loops hot enough for IC caching in the
    // default-JIT run; the restrict conversions (uncacheable dictionary +
    // flatten pin + SlowPut) must defeat whatever these loops cached.
    let sink = 0;
    for (let i = 0; i < WARM; ++i) {
        o.f = i;
        sink += o.f;
        sink += o.g;
    }
    return sink;
}

// ---- exclusions (Dev 8/11): TypeError "cannot restrict this object" ----

// Non-objects.
for (const bad of [undefined, null, 42, "x", Symbol("s"), 1n, true])
    shouldThrow(TypeError, () => Thread.restrict(bad), "cannot restrict this object");

// Global object / global proxy.
shouldThrow(TypeError, () => Thread.restrict(globalThis), "cannot restrict this object");

// Proxy.
shouldThrow(TypeError, () => Thread.restrict(new Proxy({}, {})), "cannot restrict this object");

// Species-protected builtin prototype/constructor pairs. Touch each lazy
// builtin first so its slot is materialized (the exclusion check never
// forces lazy slots; an unmaterialized slot is trivially not the receiver).
new ArrayBuffer(8);
new SharedArrayBuffer(8);
new Int8Array(4);
new Float64Array(4);
const speciesProtected = [
    Array, Array.prototype,
    Promise, Promise.prototype,
    RegExp, RegExp.prototype,
    ArrayBuffer, ArrayBuffer.prototype,
    SharedArrayBuffer, SharedArrayBuffer.prototype,
    Int8Array, Int8Array.prototype,
    Float64Array, Float64Array.prototype,
    Object.getPrototypeOf(Int8Array), // %TypedArray% (super constructor)
    Object.getPrototypeOf(Int8Array.prototype), // %TypedArray%.prototype
];
for (const o of speciesProtected)
    shouldThrow(TypeError, () => Thread.restrict(o), "cannot restrict this object");

// D13 (round 4): receivers whose ClassInfo method table overrides an
// enforced entry point bypass the 9.2-6 hooked generic paths (typed-array
// element access is keyed on TypedArrayType, StringObject serves indexed
// chars, arguments objects map indices to registers, functions reify lazy
// own properties, ...), so Thread.restrict rejects them at restrict time —
// INSTANCES, not just the species-protected prototype/constructor pairs
// above. Without this, a foreign thread could read and write every element
// of a "restricted" Float64Array with no ConcurrentAccessError.
const overriderInstances = [
    new Float64Array(4),
    new Uint8Array(4),
    new Int8Array(4),
    new BigInt64Array(2),
    new DataView(new ArrayBuffer(8)),
    new String("chars"),
    (function () { return arguments; })(1, 2, 3), // DirectArguments
    (function () { "use strict"; return arguments; })(1, 2), // ClonedArguments-family
    function f() {}, // lazy name/length/prototype via getOwnPropertySlot override
    /re/, // RegExpObject (lastIndex put/getOwnPropertySlot overrides)
];
for (const o of overriderInstances)
    shouldThrow(TypeError, () => Thread.restrict(o), "cannot restrict this object");
// Plain objects and plain arrays (the audited-delegating allowlist) remain
// restrictable — exercised throughout the rest of this file.

// ---- basic contract: returns o; owner double-restrict idempotent ----

{
    const o = { f: 0, g: "before" };
    warmUp(o);
    shouldBe(Thread.restrict(o), o, "restrict returns its argument");
    shouldBe(Thread.restrict(o), o, "owner double-restrict returns o (5.7.1-0)");
    // Owner is unaffected: values unchanged, ops still work, warm-ups pass.
    shouldBe(o.g, "before");
    shouldBe(o.f, WARM - 1);
    warmUp(o);
    shouldBe(o.f, WARM - 1);
    shouldBe("g" in o, true);
    shouldBe(Object.isExtensible(o), true);
    o[0] = "idx0"; // owner-added indexed prop stays on hooked (SlowPut) paths
    shouldBe(o[0], "idx0");
    shouldBe(delete o[0], true);
}

// ---- enforced set from a foreign thread => CAE; owner untouched ----

{
    const o = { f: 1, g: 2 };
    const arr = [10, 20, 30];
    const plain = {};
    warmUp(o);
    o.f = 1; // deterministic post-warm-up values
    Thread.restrict(o);
    Thread.restrict(arr);
    Thread.restrict(plain);
    plain[0] = "p0"; // owner adds o[0] AFTER restrict (I14 indexed-set case)

    const failures = new Thread(() => {
        const out = [];
        function expectCAE(label, fn) {
            try {
                fn();
                out.push(label + ": did not throw");
            } catch (e) {
                if (!(e instanceof ConcurrentAccessError))
                    out.push(label + ": threw " + e + " (not ConcurrentAccessError)");
            }
        }
        // Named set (full Dev-8 enforced list).
        expectCAE("get", () => o.f);
        expectCAE("set", () => { o.f = 99; });
        expectCAE("has", () => "f" in o);
        expectCAE("delete", () => delete o.f);
        expectCAE("defineProperty", () => Object.defineProperty(o, "h", { value: 3 }));
        expectCAE("ownKeys", () => Object.keys(o));
        expectCAE("ownKeys (Reflect)", () => Reflect.ownKeys(o));
        expectCAE("setPrototypeOf", () => Object.setPrototypeOf(o, null));
        expectCAE("isExtensible", () => Object.isExtensible(o));
        expectCAE("preventExtensions", () => Object.preventExtensions(o));
        // Indexed set/delete/define on an array.
        expectCAE("indexed set (array)", () => { arr[0] = 99; });
        expectCAE("indexed delete (array)", () => delete arr[1]);
        expectCAE("indexed define (array)", () => Object.defineProperty(arr, 2, { value: 99 }));
        // Indexed set on a plain {} after the owner added o[0].
        expectCAE("indexed set (plain)", () => { plain[1] = 99; });
        // Re-restrict from another thread.
        expectCAE("re-restrict", () => Thread.restrict(o));
        return out;
    }).join();
    shouldBe(failures.length, 0, "foreign-thread CAE failures: " + failures.join("; "));

    // Values unchanged by any of the (throwing) foreign ops; owner unaffected
    // and warm-ups still pass.
    shouldBe(o.f, 1);
    shouldBe(o.g, 2);
    shouldBe("f" in o, true);
    shouldBe(arr[0], 10);
    shouldBe(arr[1], 20);
    shouldBe(arr[2], 30);
    shouldBe(arr.length, 3);
    shouldBe(plain[0], "p0");
    shouldBe(plain[1], undefined);
    warmUp(o);
    shouldBe(o.f, WARM - 1);
    shouldBe(delete o.g, true);
    shouldBe("g" in o, false);
}

// ---- restrict owned by a SPAWNED thread: main thread is now foreign ----

{
    const result = new Thread(() => {
        const mine = { f: "spawned" };
        Thread.restrict(mine);
        shouldBe(Thread.restrict(mine), mine, "owner double-restrict on spawned thread");
        shouldBe(mine.f, "spawned");
        return mine;
    }).join();

    // Main thread (foreign) gets CAE on the enforced ops...
    shouldThrow(ConcurrentAccessError, () => result.f);
    shouldThrow(ConcurrentAccessError, () => { result.f = 1; });
    shouldThrow(ConcurrentAccessError, () => Thread.restrict(result), "Thread.restrict called from a non-owning thread");
    // ...even though the owning thread has already finished: restriction
    // outlives the owner (the affinity entry holds Ref<ThreadState>).
}

// ---- post-bad-time (SlowPut) array restricts OK (5.7.1(b) guard) ----

{
    const slow = [1, 2, 3];
    // An indexed accessor forces the array onto SlowPutArrayStorage, the
    // shape 5.7.1(a) no-ops on and 5.7.1(b) must NOT re-convert (CRASH).
    let setterHits = 0;
    Object.defineProperty(slow, 9, {
        get() { return "nine"; },
        set(v) { ++setterHits; },
        configurable: true,
    });
    shouldBe(slow[9], "nine");
    shouldBe(Thread.restrict(slow), slow, "post-bad-time restrict returns o");
    // Owner still fully functional, accessor intact.
    shouldBe(slow[0], 1);
    slow[9] = 42;
    shouldBe(setterHits, 1);
    shouldBe(slow[9], "nine");

    const errs = new Thread(() => {
        const out = [];
        try {
            slow[0] = 7;
            out.push("indexed set on SlowPut array: did not throw");
        } catch (e) {
            if (!(e instanceof ConcurrentAccessError))
                out.push("indexed set on SlowPut array: " + e);
        }
        return out;
    }).join();
    shouldBe(errs.length, 0, "SlowPut foreign failures: " + errs.join("; "));
    shouldBe(slow[0], 1);
}

// ---- ConcurrentAccessError shape (4.1) ----

shouldBe(typeof ConcurrentAccessError, "function");
{
    const e = new ConcurrentAccessError("m");
    shouldBe(e instanceof ConcurrentAccessError, true);
    shouldBe(e instanceof Error, true, "CAE is an Error subclass");
    shouldBe(ConcurrentAccessError.prototype.name, "ConcurrentAccessError");
}
