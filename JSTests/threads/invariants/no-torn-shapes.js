//@ requireOptions("--useJSThreads=1")
// SPEC-objectmodel invariant: no torn shapes / no structure-butterfly
// mismatch (I9, I21: "read of f returning g's value").
//
// Part 1 is the classic race from the design doc: o.f = 1 racing o.g = 2
// must never produce o.f == 2, "g" in o == false, or any mixed outcome.
// Both adds must land with their own values.
//
// Part 2: a reader thread repeatedly snapshots Object.keys while writers
// transition the object. Invariant I9 (no holes): any property visible in a
// snapshot must read back a defined, writer-intended value — a reader that
// sees the new shape must see the new property's value.
load("../resources/assert.js", "caller relative");

const ROUNDS = 64;

// Part 1: pairwise racing transitions adding distinct properties.
for (let round = 0; round < ROUNDS; ++round) {
    const o = {};
    const a = new Thread(() => { o.f = 1; });
    const b = new Thread(() => { o.g = 2; });
    a.join();
    b.join();
    shouldBeTrue("f" in o, "round " + round + ": lost f");
    shouldBeTrue("g" in o, "round " + round + ": lost g");
    shouldBe(o.f, 1, "round " + round + ": f got another property's value");
    shouldBe(o.g, 2, "round " + round + ": g got another property's value");
    shouldBe(Object.keys(o).length, 2);
}

// Part 1b: wider fan-out — each thread adds one distinct property to the
// same object, all racing the same transition chain.
for (let round = 0; round < 16; ++round) {
    const o = {};
    const workers = spawnN(6, (t) => { o["prop" + t] = t + 100; });
    joinAll(workers);
    for (let t = 0; t < 6; ++t)
        shouldBe(o["prop" + t], t + 100,
            "round " + round + ": prop" + t + " torn or lost");
    shouldBe(Object.keys(o).length, 6);
}

// Part 2: snapshot/read agreement under concurrent transitions.
// Writers add propN = "v" + N (always defined, value derivable from name).
// The reader checks every snapshotted key reads back its derived value.
{
    const o = {};
    const WRITER_PROPS = 48;
    const writers = spawnN(2, (t) => {
        for (let i = 0; i < WRITER_PROPS; ++i)
            o["w" + t + "_" + i] = "v" + t + "_" + i;
    });
    const reader = new Thread(() => {
        for (let iter = 0; iter < 400; ++iter) {
            const keys = Object.keys(o);
            for (const key of keys) {
                const v = o[key];
                // Key wN_M must read "vN_M" — undefined would be a hole
                // (structure published before value), anything else is a
                // torn shape reading another slot.
                const expected = "v" + key.substring(1);
                if (v !== expected)
                    throw new Error("snapshot saw key " + key + " but read "
                        + describe(v) + " (expected " + expected + ")");
            }
        }
        return true;
    });
    shouldBeTrue(reader.join());
    joinAll(writers);
    for (let t = 0; t < 2; ++t)
        for (let i = 0; i < WRITER_PROPS; ++i)
            shouldBe(o["w" + t + "_" + i], "v" + t + "_" + i);
}
