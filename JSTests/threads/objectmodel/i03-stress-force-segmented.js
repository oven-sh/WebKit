//@ requireOptions("--useJSThreads=1", "--forceSegmentedButterflies=1", "--verifyConcurrentButterfly=1")
// SPEC-objectmodel §9.6 stress: forceSegmentedButterflies.
//
// Every eligible allocation segments (AS/I31, CoW/I35 and butterfly-less
// objects exempt), so this runs the WHOLE mixed workload — single-threaded
// and raced — through the segmented read/write/transition/GC paths
// (§4.1-§4.5), with per-decode validation on. Behavior must be IDENTICAL to
// the unstressed runs: the stress option changes representation, never
// semantics.
//
// THREADS-INTEGRATE(objectmodel): requires integration manifest entry 1
// (OptionsList.h stress options).
load("../harness.js", "caller relative");

// Single-threaded mixed workload (mirrors the I22 twins; results must match).
{
    const o = {};
    for (let i = 0; i < 40; ++i)
        o["p" + i] = i;
    delete o.p7;
    o.p7 = "readded";
    shouldBe(o.p7, "readded");
    shouldBe(Object.keys(o).length, 40);
    for (let i = 0; i < 40; ++i) {
        if (i !== 7)
            shouldBe(o["p" + i], i);
    }

    const a = [];
    for (let i = 0; i < 500; ++i)
        a[i] = i * 2;
    shouldBe(a.length, 500);
    a.length = 100;
    shouldBe(a.length, 100);
    shouldBeFalse(400 in a);
    for (let i = 0; i < 100; ++i)
        shouldBe(a[i], i * 2);

    const d = [0.5, 1.5, 2.5];
    d[3] = 3.5;
    shouldBe(d[3], 3.5);

    gc();
    shouldBe(a[99], 198);
    shouldBe(o.p39, 39);
}

// Raced workload: disjoint adds + element growth on segmented-from-birth
// objects.
const THREADS = 4;
for (let round = 0; round < 6; ++round) {
    const o = { seed: round };
    const arr = [0];
    const workers = spawnN(THREADS, (t) => {
        for (let k = 0; k < 16; ++k)
            o["s" + t + "_" + k] = t * 100 + k;
        for (let i = t; i < 256; i += THREADS)
            arr[i] = i * 3;
        return true;
    });
    joinAll(workers).forEach(r => shouldBeTrue(r));

    shouldBe(o.seed, round);
    for (let t = 0; t < THREADS; ++t) {
        for (let k = 0; k < 16; ++k)
            shouldBe(o["s" + t + "_" + k], t * 100 + k, "round " + round + ": stressed add lost");
    }
    shouldBe(arr.length, 256, "round " + round);
    for (let i = 1; i < 256; ++i)
        shouldBe(arr[i], i * 3, "round " + round + ": stressed element lost");

    gc();
    shouldBe(o["s0_0"], 0);
    shouldBe(arr[255], 765);
}

// Exempt shapes still behave (AS + CoW under the stress flag).
{
    const sparse = [];
    sparse[1000000] = "as"; // AS: exempt from forced segmentation (I31)
    sparse[3] = "near";
    shouldBe(sparse[1000000], "as");
    shouldBe(sparse[3], "near");

    const cow = [7, 8, 9]; // CoW: exempt until materialization (I35)
    new Thread(() => { cow[1] = 80; }).join();
    shouldBe(cow[1], 80);
    shouldBe(cow[0], 7);
}
