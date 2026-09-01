//@ requireOptions("--useJSThreads=1")
// API-I15 (single-thread edges + tiered loop): the property RMW family —
// add/sub (double semantics), and/or/xor (ToInt32 both operands, int32
// result), exchange — each one atomic step, returning the OLD value as
// read. The >=1e4 Atomics.add(o,"x",1) loop runs under the default JIT
// configuration (annex §T) so the call site tiers up through the shared
// helpers (4.5 placement: tier-up can't change semantics); exact-count
// multi-thread I15 is races/counter-atomics.js.
load("../harness.js", "caller relative");

const o = { x: 5 };

// ---- add/sub: double semantics, return old value ----
shouldBe(Atomics.add(o, "x", 3), 5);
shouldBe(o.x, 8);
shouldBe(Atomics.sub(o, "x", 10), 8);
shouldBe(o.x, -2);
shouldBe(Atomics.add(o, "x", 0.5), -2);
shouldBe(o.x, -1.5);
o.x = 0.1;
shouldBe(Atomics.add(o, "x", 0.2), 0.1);
shouldBe(o.x, 0.1 + 0.2); // exact double arithmetic, not int truncation
o.x = Infinity;
shouldBe(Atomics.sub(o, "x", Infinity), Infinity);
shouldBeTrue(o.x !== o.x, "Infinity - Infinity stores NaN");
{
    o.x = NaN;
    const old = Atomics.add(o, "x", 1);
    shouldBeTrue(old !== old, "old value NaN returned as read");
    shouldBeTrue(o.x !== o.x, "NaN + 1 stores NaN");
}

// ---- and/or/xor: ToInt32 of the STORED value and the operand; the RETURN
// is the old value as read (uncoerced) ----
o.b = 6;
shouldBe(Atomics.and(o, "b", 3), 6);
shouldBe(o.b, 2);
shouldBe(Atomics.or(o, "b", 5), 2);
shouldBe(o.b, 7);
shouldBe(Atomics.xor(o, "b", 1), 7);
shouldBe(o.b, 6);
// stored double out of int32 range: returned raw, combined via ToInt32
o.b = 2147483648; // ToInt32 => -2147483648
shouldBe(Atomics.and(o, "b", -1), 2147483648, "old value returned uncoerced");
shouldBe(o.b, -2147483648, "ToInt32(stored) & ToInt32(operand), int32 result");
o.b = 1.9; // ToInt32 => 1
shouldBe(Atomics.or(o, "b", 2), 1.9);
shouldBe(o.b, 3);

// ---- operand coercion: ToNumber/ToInt32 runs (and may run JS) ----
{
    o.c = 10;
    let effects = "";
    shouldBe(Atomics.add(o, "c", { valueOf() { effects += "v"; return 2; } }), 10);
    shouldBe(effects, "v");
    shouldBe(o.c, 12);
    shouldBe(Atomics.xor(o, "c", "5"), 12); // string operand: ToInt32
    shouldBe(o.c, 9);
}

// ---- exchange: store-shaped but requires an existing own data property;
// returns the prior value; any JS value allowed ----
o.e = "before";
shouldBe(Atomics.exchange(o, "e", "after"), "before");
shouldBe(o.e, "after");
{
    const ref = {};
    shouldBe(Atomics.exchange(o, "e", ref), "after");
    shouldBe(Atomics.exchange(o, "e", 1), ref);
    shouldBe(o.e, 1);
}

// ---- indexed keys ----
{
    const arr = [1, 2, 3];
    shouldBe(Atomics.add(arr, 1, 10), 2);
    shouldBe(arr[1], 12);
    shouldBe(Atomics.exchange(arr, 0, "swapped"), 1);
    shouldBe(arr[0], "swapped");
}

// ---- tiered loop: >=1e4 Atomics.add(o,"x",1), default JIT (annex §T).
// The count must be exact after the loop crosses tier-up thresholds. ----
{
    o.x = 0;
    const ITERS = 2e4;
    for (let i = 0; i < ITERS; ++i)
        Atomics.add(o, "x", 1);
    shouldBe(o.x, ITERS);
}

// ---- tiered exchange/sub loops keep returning the exact old value ----
{
    o.x = 0;
    for (let i = 0; i < 1e4; ++i) {
        const old = Atomics.exchange(o, "x", i + 1);
        if (old !== i)
            throw new Error("exchange old value wrong at " + i + ": " + old);
    }
    shouldBe(o.x, 1e4);
    for (let i = 0; i < 1e4; ++i)
        Atomics.sub(o, "x", 1);
    shouldBe(o.x, 0);
}
