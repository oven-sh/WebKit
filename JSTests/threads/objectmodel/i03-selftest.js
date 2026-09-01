//@ requireOptions("--useJSThreads=1", "--verifyConcurrentButterfly=1")
// SPEC-objectmodel Task 12: C++ self-test driver.
//
// verifyConcurrentButterfly=1 makes VM startup run concurrentButterflySelfTest
// + concurrentButterflyStressSelfTest (ConcurrentButterfly.h/.cpp): §9.1
// encode/decode round trips, §9.2 DCAS semantics + lock-freedom RELEASE_ASSERT
// (I32), the PA-alignment lane witness (I36), and the §3.0 volatile-byte merge
// loop. If any of those RELEASE_ASSERTs fail the shell crashes before this
// script's first statement, so reaching the end of this file IS the pass
// signal for the C++ legs.
//
// The body below additionally exercises every tag-decode path with the
// per-decode validateTaggedButterflyWord checks live (I2/I3 + the butterfly()
// flatness contract assert), across all regimes a mutator can produce:
// None -> Flat (N3 install), Flat -> FlatShared (F1 SW flip), Flat ->
// Segmented (§4.2 conversion via foreign transition), array growth (§4.4 CAS,
// T5), Double shape (§4.7), CoW (§4.8), ArrayStorage (§4.6), dictionary +
// delete (§6), and GC visitation of all of the above (§4.5).
//
// THREADS-INTEGRATE(objectmodel): requires integration manifest entry 1
// (OptionsList.h --verifyConcurrentButterfly). Until entry 1 lands, the
// option probe compiles dark and this test is the first to fail option
// parsing — that failure mode is expected pre-integration.
load("../harness.js", "caller relative");

// --- None -> Flat -> FlatShared -> Segmented on one object ---
const o = { a: 1, b: 2 }; // inline
for (let i = 0; i < 12; ++i)
    o["f" + i] = i * 3; // N3 install + flat out-of-line growth (decodes validated)

new Thread(() => { o.f0 = -1; }).join();          // F1 foreign write: SW flip
new Thread(() => { o.added = "foreign"; }).join(); // foreign transition: §4.2 conversion
for (let i = 0; i < 24; ++i)
    o["g" + i] = "post" + i; // segmented growth (§4.3) with validation on

shouldBe(o.a, 1);
shouldBe(o.f0, -1);
shouldBe(o.added, "foreign");
for (let i = 1; i < 12; ++i)
    shouldBe(o["f" + i], i * 3);
for (let i = 0; i < 24; ++i)
    shouldBe(o["g" + i], "post" + i);

// --- Array regimes: contiguous growth, Double, CoW, ArrayStorage ---
const arr = [];
for (let i = 0; i < 200; ++i)
    arr[i] = i; // T1/T5 growth, decodes validated
new Thread(() => { arr[0] = 1000; arr[200] = 200; }).join(); // foreign write + grow
shouldBe(arr[0], 1000);
shouldBe(arr[200], 200);
shouldBe(arr.length, 201);

const dbl = [0.5, 1.5, 2.5];
new Thread(() => { dbl[1] = 9.25; }).join(); // shared Double stays Double (§4.7)
shouldBe(dbl[1], 9.25);
dbl.push("not-a-double"); // Double -> Contiguous relabel
shouldBe(dbl[3], "not-a-double");

const cow = [1, 2, 3]; // CoW literal
new Thread(() => { cow[0] = 7; }).join(); // §4.8 materialize-first
shouldBe(cow[0], 7);
shouldBe(cow[2], 3);

const sparse = [];
sparse[1000000] = "far"; // ArrayStorage with sparse map (§4.6, I31)
new Thread(() => { sparse[5] = "near"; }).join(); // every AS access cell-locked
shouldBe(sparse[5], "near");
shouldBe(sparse[1000000], "far");

// --- Dictionary + delete + GC visitation with validation live ---
const dict = {};
for (let i = 0; i < 64; ++i)
    dict["k" + i] = i;
for (let i = 0; i < 32; ++i)
    delete dict["k" + (i * 2)];
dict.readd = true;
gc();
gc();
shouldBe(dict.k1, 1);
shouldBe(dict.k0, undefined);
shouldBeTrue(dict.readd);
shouldBe(o.added, "foreign"); // segmented object survived GC (§4.5)
shouldBe(arr[200], 200);
