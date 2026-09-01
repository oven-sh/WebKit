//@ requireOptions("--useJSThreads=1")
// SPEC-api 4.5 compareExchange(o, k, expected, replacement) compares with
// SameValueZero — NaN matches NaN and +0 matches -0 (=== would break NaN
// CAS retry loops) — and returns the value READ either way.
load("../harness.js", "caller relative");

const o = {};

// ---- NaN matches NaN ----
o.k = NaN;
{
    const old = Atomics.compareExchange(o, "k", NaN, 1);
    shouldBeTrue(old !== old, "returns the NaN that was read");
    shouldBe(o.k, 1, "SVZ(NaN, NaN) is true: replacement stored");
}
// ...including a differently-produced NaN
o.k = 0 / 0;
{
    const old = Atomics.compareExchange(o, "k", Number.NaN, "hit");
    shouldBeTrue(old !== old, "returned the stored NaN");
    shouldBe(o.k, "hit");
}

// ---- +0 / -0 match under SVZ ----
o.z = -0;
shouldBe(Atomics.compareExchange(o, "z", 0, "zhit"), -0); // returns -0 as read
shouldBe(o.z, "zhit");
o.z = 0;
shouldBe(Atomics.compareExchange(o, "z", -0, "zhit2"), 0);
shouldBe(o.z, "zhit2");

// ---- mismatch: no store, returns current ----
o.m = 5;
shouldBe(Atomics.compareExchange(o, "m", 6, 7), 5);
shouldBe(o.m, 5);
// NaN expected vs non-NaN current: mismatch
shouldBe(Atomics.compareExchange(o, "m", NaN, 7), 5);
shouldBe(o.m, 5);

// ---- objects compare by identity ----
{
    const ref = { tag: 1 };
    o.r = ref;
    shouldBe(Atomics.compareExchange(o, "r", { tag: 1 }, "no"), ref, "structural twin must not match");
    shouldBe(o.r, ref);
    shouldBe(Atomics.compareExchange(o, "r", ref, "yes"), ref);
    shouldBe(o.r, "yes");
}

// ---- strings compare by value (SVZ -> string equality, ropes resolved) ----
o.s = "abc";
shouldBe(Atomics.compareExchange(o, "s", "a" + "bc", "swapped"), "abc");
shouldBe(o.s, "swapped");
shouldBe(Atomics.compareExchange(o, "s", "SWAPPED", "no"), "swapped"); // case-sensitive mismatch
shouldBe(o.s, "swapped");

// ---- booleans / undefined / null / bigint ----
o.b = false;
shouldBe(Atomics.compareExchange(o, "b", false, true), false);
shouldBe(o.b, true);
o.u = undefined;
shouldBe(Atomics.compareExchange(o, "u", undefined, "set"), undefined);
shouldBe(o.u, "set");
o.n = null;
shouldBe(Atomics.compareExchange(o, "n", null, "nset"), null);
shouldBe(o.n, "nset");
o.big = 10n;
shouldBe(Atomics.compareExchange(o, "big", 10n, 11n), 10n);
shouldBe(o.big, 11n);
// SVZ does NOT loosely coerce: number 10 must not match bigint 11n
shouldBe(Atomics.compareExchange(o, "big", 10, "no"), 11n);
shouldBe(o.big, 11n);

// ---- the canonical NaN-tolerant CAS retry loop (I15 shape, single-thread):
// must terminate in one round per step even when the slot holds NaN ----
{
    o.acc = NaN;
    let rounds = 0;
    for (let step = 0; step < 10; ++step) {
        for (;;) {
            ++rounds;
            if (rounds > 100)
                throw new Error("CAS retry loop failed to make progress");
            const cur = Atomics.load(o, "acc");
            const next = (cur !== cur) ? 1 : cur + 1;
            const seen = Atomics.compareExchange(o, "acc", cur, next);
            // success iff what we saw is SVZ-equal to what we read
            if (seen === cur || (seen !== seen && cur !== cur))
                break;
        }
    }
    shouldBe(o.acc, 10);
    shouldBe(rounds, 10, "uncontended CAS must succeed first try each step");
}
