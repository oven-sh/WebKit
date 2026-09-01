//@ requireOptions("--useJSThreads=1")
// MC-REENT S1 pin (docs/threads/cve/map-MC-REENT.md): SPEC-api 4.5 makes
// every property-Atomics op "one atomic step", which holds only if every
// user-JS-capable coercion (ToPropertyKey on the key, ToNumber/ToInt32 on
// the RMW operand) is sequenced strictly BEFORE the own-property
// read+validate+write step (ThreadAtomics.cpp coercion-first ordering;
// AtomicsObject.cpp ToPropertyKey-before-probe).
//
// Each case below has an exact expected outcome under that ordering and a
// DIFFERENT exact outcome if a coercion ever migrates inside the step, so
// this is a deterministic regression pin — valid under the phase-1 GIL and
// unchanged post-GIL (GI). Single-threaded on purpose: the mechanism class
// is same-thread re-entrancy; the cross-thread twin is covered by
// mc-reent-store-missing-indexed-define-race.js and the races/ suites.
load("../harness.js", "caller relative");

// Case 1: RMW operand valueOf mutates the target slot. Coercion-first means
// the side effect lands BEFORE the atomic read: add reads 100, returns 100,
// stores 101. If the operand were coerced between read and write, add would
// have read 0 (returning 0) and stored 1 or 101 depending on the breakage.
{
    const o = { x: 0 };
    const old = Atomics.add(o, "x", { valueOf() { o.x = 100; return 1; } });
    shouldBe(old, 100, "add must read AFTER operand coercion side effects");
    shouldBe(o.x, 101, "add result must be computed from the post-coercion value");
}

// Case 2: operand valueOf DELETES the property. The post-coercion probe must
// classify Missing and throw the precise RMW TypeError; pre-coercion
// validation would instead succeed against the stale slot.
{
    const o = { x: 7 };
    shouldThrow(TypeError, () => Atomics.add(o, "x", { valueOf() { delete o.x; return 1; } }));
    shouldBeFalse("x" in o, "deletion from the coercion must be visible to the step");
}

// Case 3: operand valueOf reconfigures the slot to an ACCESSOR. The
// post-coercion probe must classify Accessor (TypeError), never CAS/store a
// number over a GetterSetter (the S2/U-T10 type-confusion shape).
{
    const o = { x: 1 };
    let getterCalls = 0;
    shouldThrow(TypeError, () => Atomics.sub(o, "x", {
        valueOf() {
            Object.defineProperty(o, "x", { get() { getterCalls++; return 42; }, configurable: true });
            return 1;
        }
    }));
    shouldBe(o.x, 42, "the accessor installed during coercion must survive intact");
    shouldBe(getterCalls, 1, "RMW must not have invoked or replaced the getter during the step");
}

// Case 4: key ToPropertyKey side effect deletes the named slot. The key is
// coerced before the probe, so load must see the post-side-effect object and
// throw its precise "no own property" TypeError.
{
    const o = { k: 5 };
    shouldThrow(TypeError, () => Atomics.load(o, { toString() { delete o.k; return "k"; } }));
}

// Case 5: key coercion ADDS the slot. Probe runs after coercion => the load
// must succeed and see the just-added value (no stale Missing verdict).
{
    const o = {};
    shouldBe(Atomics.load(o, { toString() { o.k = 9; return "k"; } }), 9,
        "probe must run on post-coercion state");
}

// Case 6: bitwise RMW (ToInt32 leg) — operand coercion freezes the object.
// Coercion-first: the probe then sees a ReadOnly slot and throws the
// writability TypeError; the slot value must be untouched.
{
    const o = { x: 3 };
    shouldThrow(TypeError, () => Atomics.or(o, "x", { valueOf() { Object.freeze(o); return 4; } }));
    shouldBe(o.x, 3, "a frozen slot must never be mutated in place");
}

// Case 7: wait timeout ToNumber runs before the step-1 read
// (ThreadAtomics.cpp parseAtomicsTimeout-before-load). The timeout's
// valueOf changes the waited-on value; the read must see the NEW value and
// report "not-equal" instead of parking on the stale one.
{
    const o = { v: 0 };
    const r = Atomics.wait(o, "v", 0, { valueOf() { o.v = 1; return 50; } });
    shouldBe(r, "not-equal", "wait must read the slot AFTER timeout coercion");
}

// Case 8: Proxy receivers are rejected up front (S2 Gate 1) — the trap must
// never run inside (or before) the step.
{
    let trapped = 0;
    const p = new Proxy({ x: 1 }, { getOwnPropertyDescriptor() { trapped++; return undefined; } });
    shouldThrow(TypeError, () => Atomics.load(p, "x"));
    shouldThrow(TypeError, () => Atomics.add(p, "x", 1));
    shouldThrow(TypeError, () => Atomics.store(p, "x", 1));
    shouldBe(trapped, 0, "no proxy trap may run from a property-Atomics op");
}
