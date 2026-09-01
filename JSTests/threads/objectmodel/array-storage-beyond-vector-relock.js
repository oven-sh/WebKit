//@ requireOptions("--useJSThreads=1")
// Flag-on, the ArrayStorage beyond-vector put paths (putByIndex and
// putDirectIndex) re-read the vector bound under the cell lock after
// increaseVectorLength, install a fresh sparse map only while the header still
// has none (adopting a racer's map otherwise), and re-validate both before the
// map->vector consolidation. Single-threaded coverage of each leg, then the
// same legs driven from a foreign thread, then a preventExtensions'd array
// whose sparse-mode map must survive concurrent beyond-vector stores.
load("../harness.js", "caller relative");

function defineData(o, i, v) {
    Object.defineProperty(o, i, { value: v, writable: true, enumerable: true, configurable: true });
}

// Vector growth just past the vector, then a sparse-worthy index that installs
// the map, then dense fills that consolidate the map back into the vector.
{
    const a = [];
    a[100000] = "far"; // ArrayStorage with a sparse map.
    for (let i = 0; i < 8; ++i)
        a[i] = i;
    a[20] = "grow"; // Beyond the vector: grows it.
    shouldBe(a[20], "grow");
    shouldBe(a[7], 7);
    shouldBe(a.length, 100001);
    a[100001] = "far2"; // Into the map.
    shouldBe(a[100001], "far2");
    shouldBe(a.length, 100002);
    defineData(a, 21, "defined");
    shouldBe(a[21], "defined");
    defineData(a, 100002, "farDefined");
    shouldBe(a[100002], "farDefined");
    shouldBe(Object.keys(a).join(), "0,1,2,3,4,5,6,7,20,21,100000,100001,100002");
}

// Fresh sparse map from the no-map leg of both paths, then consolidation: the
// entries end up in the vector and every value is intact.
{
    const a = [];
    a[100000] = "far";
    delete a[100000]; // Map stays installed but empty; the vector is small.
    for (let i = 0; i < 4; ++i)
        a[i] = i;
    a[50000] = "sparse"; // Too sparse for the vector: goes into the map.
    shouldBe(a[50000], "sparse");
    for (let i = 4; i < 50000; ++i)
        a[i] = i; // Dense enough: the map is consolidated into the vector.
    shouldBe(a[50000], "sparse");
    shouldBe(a[49999], 49999);
    shouldBe(a[4], 4);
    shouldBe(a.length, 100001);
    shouldBeFalse(a.hasOwnProperty(100000));

    const b = [];
    b[100000] = "far";
    delete b[100000];
    for (let i = 0; i < 4; ++i)
        defineData(b, i, i);
    defineData(b, 50000, "sparse");
    shouldBe(b[50000], "sparse");
    for (let i = 4; i < 50000; ++i)
        defineData(b, i, i);
    shouldBe(b[50000], "sparse");
    shouldBe(b[49999], 49999);
    shouldBe(b.length, 100001);
}

// The same legs from a foreign thread: the first foreign ArrayStorage write
// flips SW under a stop, then every header edit is cell-locked.
{
    const a = [];
    a[100000] = "far";
    for (let i = 0; i < 8; ++i)
        a[i] = i;
    const b = [];
    b[100000] = "far";
    delete b[100000];
    for (let i = 0; i < 4; ++i)
        b[i] = i;
    const t = new Thread(() => {
        a[20] = "grow";
        a[100001] = "far2";
        defineData(a, 21, "defined");
        defineData(a, 100002, "farDefined");
        b[50000] = "sparse";
        for (let i = 4; i < 50000; ++i)
            b[i] = i;
        return a[20] === "grow" && a[100001] === "far2" && a[21] === "defined" && a[100002] === "farDefined"
            && b[50000] === "sparse" && b[49999] === 49999;
    });
    shouldBeTrue(t.join());
    shouldBe(a[20], "grow");
    shouldBe(a[7], 7);
    shouldBe(a[100000], "far");
    shouldBe(a[100001], "far2");
    shouldBe(a[21], "defined");
    shouldBe(a[100002], "farDefined");
    shouldBe(a.length, 100003);
    shouldBe(b[50000], "sparse");
    shouldBe(b[4], 4);
    shouldBe(b[49999], 49999);
    shouldBe(b.length, 100001);
    a[22] = "owner";
    shouldBe(a[22], "owner");
    shouldBe(Object.keys(a).join(), "0,1,2,3,4,5,6,7,20,21,22,100000,100001,100002");
}

// A non-extensible ArrayStorage array holds every element in its sparse-mode
// map. Foreign beyond-vector stores must neither grow the vector nor replace
// that map; in-bounds stores to existing elements still land.
{
    const a = [];
    a[100000] = "far";
    for (let i = 0; i < 8; ++i)
        a[i] = i;
    Object.preventExtensions(a);
    shouldBeFalse(Object.isExtensible(a));
    const workers = spawnN(4, (t) => {
        for (let round = 0; round < 16; ++round) {
            a[200 + t * 16 + round] = "new"; // Rejected silently (sloppy mode).
            a[t] = "t" + t; // Existing element: allowed.
        }
        let threw = false;
        try {
            (function() { "use strict"; a[5000 + t] = "strict"; })();
        } catch (e) {
            threw = e instanceof TypeError;
        }
        return threw;
    });
    joinAll(workers).forEach(r => shouldBeTrue(r));
    for (let t = 0; t < 4; ++t)
        shouldBe(a[t], "t" + t);
    for (let i = 4; i < 8; ++i)
        shouldBe(a[i], i);
    shouldBe(a[100000], "far");
    shouldBe(a.length, 100001);
    shouldBe(Object.keys(a).join(), "0,1,2,3,4,5,6,7,100000");
    shouldBeFalse(a.hasOwnProperty(200));
    shouldBeFalse(a.hasOwnProperty(5000));
}

// Racing growers on one ArrayStorage array: every store lands, and every
// index each thread owned reads back its value.
{
    const THREADS = 4;
    const a = [];
    a[100000] = "far";
    a[0] = 0;
    const workers = spawnN(THREADS, (t) => {
        for (let i = 1 + t; i < 2048; i += THREADS)
            a[i] = i;
        for (let i = 0; i < 8; ++i)
            defineData(a, 100001 + t * 8 + i, "d" + t + "_" + i);
        return true;
    });
    joinAll(workers).forEach(r => shouldBeTrue(r));
    for (let i = 0; i < 2048; ++i)
        shouldBe(a[i], i, "index " + i);
    for (let t = 0; t < THREADS; ++t) {
        for (let i = 0; i < 8; ++i)
            shouldBe(a[100001 + t * 8 + i], "d" + t + "_" + i);
    }
    shouldBe(a[100000], "far");
    shouldBe(a.length, 100001 + THREADS * 8);
}
