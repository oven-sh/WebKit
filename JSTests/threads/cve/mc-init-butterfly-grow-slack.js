//@ requireOptions("--useJSThreads=1", "--forceButterflySWBit=1")
// MC-INIT surface 2 regression (docs/threads/cve/map-MC-INIT.md):
// butterfly growth must never expose tryCreateUninitialized slack.
//
// SPEC-objectmodel's only true MC-INIT hole was T5 in-place vectorLength
// growth, REMOVED in adversarial review (ConcurrentButterfly.cpp:
// 2358-2375): a lock-free foreign reader's vectorLength load -> slot
// load edge is a CONTROL dependency only (same base pointer), which does
// not order load->load on arm64, so an in-place bound raise could pair a
// post-growth length with a pre-hole-fill slot load and lift
// uninitialized slack into a JSValue. The fix shape: every growth
// publishes FRESH fully-initialized storage behind a new butterfly-word
// load (T1 fence+CAS at :2402-2412; spine slack clears at :1944/:566).
// This test pins that property against regression (e.g. a future
// re-introduction of in-place growth).
//
// --forceButterflySWBit routes owner writes through the foreign/SW path,
// so growth lands on conversion + segmented T2 spine publication (§4.2/
// §4.3) — the publication-heaviest growth form. A flag-only companion
// run (amplifier config) exercises the flat T1 copy path.
//
// Owner thread appends i -> arr[i] = i (int lane) and d[i] = i + 0.5
// (double lane, PNaN-hole slack). Reader threads continuously sample:
// for any index j, arr[j] must be exactly j or undefined (not yet
// written / beyond publicLength); d[j] must be j + 0.5 or undefined
// (PNaN slack reads as a hole). ANY other observed value is
// uninitialized-slack disclosure or a torn publication.
//
// EXECUTE POST-UNGIL ONLY. Amplifier-ready: growth happens continuously,
// so every quantum carries republication edges (also run under
// --forceSegmentedButterflies=1 and TSAN no-JIT via Tools/threads/
// amplify.sh).
load("../harness.js", "caller relative");

const N = 4; // reader threads
const TARGET = 50000;

const shared = { arr: [], d: [], stop: 0, started: 0, go: 0 };

const readers = spawnN(N, (index) => {
    Atomics.add(shared, "started", 1);
    while (Atomics.load(shared, "go") === 0)
        Atomics.wait(shared, "go", 0, 100);

    const arr = shared.arr;
    const d = shared.d;
    let failures = 0;
    let seed = 0x9e3779b9 ^ index;
    while (!Atomics.load(shared, "stop")) {
        // xorshift sampler — probe a spread of indices including ones at
        // and beyond the racing frontier (length is racing upward).
        seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5; seed >>>= 0;
        const len = arr.length;
        const j = seed % (len + 64); // deliberately samples past length too

        const v = arr[j];
        if (v !== undefined && v !== j)
            ++failures; // alien value: slack disclosure or torn publish

        const w = d[j];
        if (w !== undefined && w !== j + 0.5)
            ++failures; // double lane: PNaN slack must read as a hole
    }
    return failures;
});

waitUntil(() => Atomics.load(shared, "started") === N);
Atomics.store(shared, "go", 1);
Atomics.notify(shared, "go");

const arr = shared.arr;
const d = shared.d;
for (let i = 0; i < TARGET; ++i) {
    arr[i] = i;        // dense int append: ensureLength growth path
    d[i] = i + 0.5;    // dense double append: raw-double fragments (§4.7)
    if ((i & 4095) === 0)
        sleepMs(1); // cooperative yield so readers overlap every growth band
}
Atomics.store(shared, "stop", 1);

for (const failures of joinAll(readers))
    shouldBe(failures, 0, "uninitialized butterfly slack or torn growth publication observed (MC-INIT surface 2)");

// Post-race determinism: final contents are exactly the appended values.
shouldBe(arr.length, TARGET);
shouldBe(d.length, TARGET);
for (let i = 0; i < TARGET; i += 977) {
    shouldBe(arr[i], i);
    shouldBe(d[i], i + 0.5);
}
