//@ requireOptions("--useJSThreads=1")
// Atomics extended to ordinary object properties: load/store/RMW/
// compareExchange semantics, sameValueZero matching, error surface, and the
// unchanged typed-array path. Single-threaded except for one shared-counter
// stampede; all timed waits run with no other thread alive.
load("../resources/assert.js", "caller relative");

const o = { x: 0 };

// ---- load/store ----
shouldBe(Atomics.load(o, "x"), 0);
shouldBe(Atomics.store(o, "x", 7), 7, "store returns the stored value");
shouldBe(o.x, 7);
shouldBe(Atomics.load(o, "x"), 7);

// store can create a new own property (and returns the stored value).
shouldBe(Atomics.store(o, "fresh", "created"), "created");
shouldBe(o.fresh, "created");

// Any JS value can be stored/loaded/exchanged.
const ref = { tag: "ref" };
Atomics.store(o, "x", ref);
shouldBe(Atomics.load(o, "x"), ref);
shouldBe(Atomics.exchange(o, "x", undefined), ref, "exchange returns the old value");
shouldBe(Atomics.load(o, "x"), undefined);
Atomics.store(o, "x", 10);

// ---- numeric RMW ops return the old value and apply the operation ----
shouldBe(Atomics.add(o, "x", 5), 10);
shouldBe(o.x, 15);
shouldBe(Atomics.sub(o, "x", 3), 15);
shouldBe(o.x, 12);
shouldBe(Atomics.and(o, "x", 10), 12);   // 0b1100 & 0b1010
shouldBe(o.x, 8);
shouldBe(Atomics.or(o, "x", 3), 8);
shouldBe(o.x, 11);
shouldBe(Atomics.xor(o, "x", 6), 11);    // 0b1011 ^ 0b0110
shouldBe(o.x, 13);

// RMW on a non-numeric stored value throws and leaves it unchanged.
Atomics.store(o, "x", "string");
shouldThrow(TypeError, () => Atomics.add(o, "x", 1));
shouldBe(o.x, "string");
Atomics.store(o, "x", 1);

// ---- compareExchange: sameValueZero on arbitrary values ----
shouldBe(Atomics.compareExchange(o, "x", 1, 2), 1, "match swaps, returns old");
shouldBe(o.x, 2);
shouldBe(Atomics.compareExchange(o, "x", 999, 3), 2, "mismatch returns current");
shouldBe(o.x, 2, "mismatch does not swap");
// NaN matches NaN.
Atomics.store(o, "x", NaN);
shouldBe(Atomics.compareExchange(o, "x", NaN, "swapped-nan"), NaN);
shouldBe(o.x, "swapped-nan");
// Object identity, not structural equality.
Atomics.store(o, "x", ref);
shouldBe(Atomics.compareExchange(o, "x", { tag: "ref" }, "nope"), ref);
shouldBe(o.x, ref, "structurally-equal object must not match");
shouldBe(Atomics.compareExchange(o, "x", ref, "yes"), ref);
shouldBe(o.x, "yes");
// Strings compare by value.
shouldBe(Atomics.compareExchange(o, "x", "y" + "es", 0), "yes");
shouldBe(o.x, 0);

// ---- error surface ----
// load/RMW/compareExchange require an existing own data property.
shouldThrow(TypeError, () => Atomics.load(o, "missing"));
shouldThrow(TypeError, () => Atomics.add(o, "missing", 1));
shouldThrow(TypeError, () => Atomics.exchange(o, "missing", 1));
shouldThrow(TypeError, () => Atomics.compareExchange(o, "missing", 0, 1));
// Inherited properties don't count.
const child = Object.create({ inherited: 1 });
shouldThrow(TypeError, () => Atomics.load(child, "inherited"));
// Accessors don't count.
const acc = {};
Object.defineProperty(acc, "g", { get() { return 1; }, set() {} });
shouldThrow(TypeError, () => Atomics.load(acc, "g"));
shouldThrow(TypeError, () => Atomics.add(acc, "g", 1));
// Non-object receivers.
shouldThrow(TypeError, () => Atomics.load(null, "x"));
shouldThrow(TypeError, () => Atomics.store(undefined, "x", 1));
shouldThrow(TypeError, () => Atomics.add(7, "x", 1));

// ---- wait/waitAsync/notify value semantics (no other threads alive) ----
const cell = { v: 0 };
// Mismatched expected value: returns "not-equal" without blocking.
shouldBe(Atomics.wait(cell, "v", 1), "not-equal");
// Matched + zero timeout: "timed-out" without a wakeup.
shouldBe(Atomics.wait(cell, "v", 0, 0), "timed-out");
// Short real timeout.
shouldBe(Atomics.wait(cell, "v", 0, 30), "timed-out");
// sameValueZero matching rules for the expected value.
Atomics.store(cell, "v", NaN);
shouldBe(Atomics.wait(cell, "v", NaN, 0), "timed-out", "NaN matches NaN");
Atomics.store(cell, "v", 0);
shouldBe(Atomics.wait(cell, "v", -0, 0), "timed-out", "-0 matches +0");
Atomics.store(cell, "v", ref);
shouldBe(Atomics.wait(cell, "v", ref, 0), "timed-out", "object identity matches");
shouldBe(Atomics.wait(cell, "v", { tag: "ref" }, 0), "not-equal");
Atomics.store(cell, "v", 0);

// notify with no waiters wakes nobody, whatever the count.
shouldBe(Atomics.notify(cell, "v"), 0);
shouldBe(Atomics.notify(cell, "v", 0), 0);
shouldBe(Atomics.notify(cell, "v", 99), 0);

// waitAsync returns a result object; the not-equal case is sync.
const notEqual = Atomics.waitAsync(cell, "v", 123);
shouldBeFalse(notEqual.async);
shouldBe(notEqual.value, "not-equal");

// ---- typed-array path is unchanged ----
const i32 = new Int32Array(new SharedArrayBuffer(16));
shouldBe(Atomics.store(i32, 0, 41), 41);
shouldBe(Atomics.add(i32, 0, 1), 41);
shouldBe(Atomics.load(i32, 0), 42);
shouldBe(Atomics.compareExchange(i32, 0, 42, 7), 42);
shouldBe(i32[0], 7);
shouldBe(Atomics.wait(i32, 1, 999), "not-equal");
shouldBe(Atomics.notify(i32, 1), 0);

// ---- cross-thread atomicity: racing RMW on an object property ----
const counter = { n: 0, mask: 0 };
const THREADS = 4;
const LAPS = 500;
const adders = spawnN(THREADS, index => {
    for (let i = 0; i < LAPS; ++i)
        Atomics.add(counter, "n", 1);
    Atomics.or(counter, "mask", 1 << index);
    return Atomics.load(counter, "n") > 0;
});
joinAll(adders).forEach(saw => shouldBeTrue(saw));
shouldBe(counter.n, THREADS * LAPS, "no lost atomic increments");
shouldBe(counter.mask, (1 << THREADS) - 1, "every thread's OR landed");

// compareExchange race: exactly one thread wins a claim.
const claim = { owner: -1, winners: 0 };
const claimers = spawnN(THREADS, index => {
    if (Atomics.compareExchange(claim, "owner", -1, index) === -1) {
        Atomics.add(claim, "winners", 1);
        return "won";
    }
    return "lost";
});
const outcomes = joinAll(claimers);
shouldBe(claim.winners, 1, "exactly one compareExchange winner");
shouldBe(outcomes.filter(r => r === "won").length, 1);
shouldBeTrue(claim.owner >= 0 && claim.owner < THREADS);
