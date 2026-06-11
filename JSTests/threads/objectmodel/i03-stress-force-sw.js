//@ requireOptions("--useJSThreads=1", "--forceButterflySWBit=1", "--verifyConcurrentButterfly=1")
// SPEC-objectmodel §9.6 stress: forceButterflySWBit.
//
// Every write is treated as foreign (§3 DCAS + F1 firing): even the
// allocating thread's own stores go through the SW-flip path, every owner
// transition behaves like "owner with SW=1" (=> segmented per §3), and the
// TTL writeThreadLocal sets fire eagerly. Semantics must be unchanged; this
// is the densest exerciser of the F1 fire-then-DCAS + §3.0 merge loop +
// conversion pipeline.
//
// THREADS-INTEGRATE(objectmodel): requires integration manifest entry 1
// (OptionsList.h stress options).
load("../harness.js", "caller relative");

// Single-threaded: every store below takes the stressed foreign-write path.
{
    const o = { a: 1 };
    for (let i = 0; i < 32; ++i)
        o["w" + i] = "v" + i;
    for (let i = 0; i < 32; ++i)
        o["w" + i] = "v2_" + i; // existing-slot rewrites: SW DCAS path each round
    for (let i = 0; i < 32; ++i)
        shouldBe(o["w" + i], "v2_" + i);
    shouldBe(o.a, 1);

    const arr = [];
    for (let i = 0; i < 300; ++i)
        arr[i] = i; // stressed writes: resize CASes under forced-foreign rules
    shouldBe(arr.length, 300);
    for (let i = 0; i < 300; ++i)
        shouldBe(arr[i], i);

    const d = [1.5, 2.5];
    d[0] = 9.5; // Double raw-store path under forced SW
    shouldBe(d[0], 9.5);
    shouldBe(d[1], 2.5);

    delete o.w5;
    shouldBeFalse("w5" in o);
    o.w5 = "back";
    shouldBe(o.w5, "back");

    gc();
    shouldBe(o.w31, "v2_31");
    shouldBe(arr[299], 299);
}

// Raced: with forced SW, BOTH sides of every race run the shared-write
// protocol; disjoint adds/writes must still all land (I21 under maximum
// protocol coverage).
const THREADS = 4;
for (let round = 0; round < 6; ++round) {
    const o = {};
    const workers = spawnN(THREADS, (t) => {
        for (let k = 0; k < 24; ++k)
            o["f" + t + "_" + k] = (t << 16) | k;
        return true;
    });
    joinAll(workers).forEach(r => shouldBeTrue(r));
    for (let t = 0; t < THREADS; ++t) {
        for (let k = 0; k < 24; ++k)
            shouldBe(o["f" + t + "_" + k], (t << 16) | k, "round " + round + ": forced-SW add lost");
    }
    shouldBe(Object.keys(o).length, THREADS * 24);
}

// Same-name racing writes under forced SW: last-value-wins per slot among
// the written set; never torn.
for (let round = 0; round < 6; ++round) {
    const o = { slot: "init" };
    const writers = spawnN(THREADS, (t) => {
        for (let k = 0; k < 50; ++k)
            o.slot = "t" + t + "_" + k;
        return true;
    });
    joinAll(writers).forEach(r => shouldBeTrue(r));
    const v = o.slot;
    if (!/^t[0-3]_49$/.test(String(v)))
        throw new Error("round " + round + ": final same-name value not a last write: " + describe(v));
}
