//@ requireOptions("--useJSThreads=1")
// Annex §15.T12 suite: stale-spine reader vs T2 grow+push — I33, BOTH
// clauses (indexed and out-of-line).
//
// C4/I33: publicLength lives in fragment 0 slot 0 and is SHARED by every
// spine the object ever publishes, so a reader holding a SUPERSEDED spine
// can observe publicLength > itsSpine->vectorLength. Every segmented access
// must bound by min(publicLength, loadedSpine->vectorLength) and treat
// [vectorLength, publicLength) as holes; out-of-line reads must bound
// offsetInOutOfLineStorage < 4*outOfLineFragmentCount and re-load/re-dispatch
// when out of range. Observable from JS: while a grower pushes elements and
// adds properties (new spines via T2/§4.3), a racing reader of high indexes
// and late properties sees EITHER undefined (hole/stale spine) or the
// writer's exact value — never garbage, never a crash, never another slot's
// value.
load("../harness.js", "caller relative");

const ROUNDS = 6;
const TARGET = 800;        // indexed growth: many spine replacements
const PROPS = 64;          // out-of-line growth: several fragment-count steps
const SAMPLES = 600;

for (let round = 0; round < ROUNDS; ++round) {
    const a = [0];
    // Force segmentation up front: one foreign add converts (any later spine
    // is a T2/§4.3 replacement, which is what the stale reader must tolerate).
    new Thread(() => { a.seed = "segmented"; }).join();
    const gate = { go: 0 };

    const reader = new Thread(() => {
        while (Atomics.load(gate, "go") === 0)
            sleepMs(1);
        for (let s = 0; s < SAMPLES; ++s) {
            // I33 indexed clause: read a sliding high index, possibly beyond
            // the loaded spine's vectorLength.
            const i = (s * 13) % TARGET;
            const v = a[i];
            if (v !== undefined && v !== i * 3)
                throw new Error("round " + round + ": indexed stale-spine read a[" + i + "] = "
                    + describe(v) + " (I33 indexed)");
            // length monotonically grows; never exceeds the final target.
            const len = a.length;
            if (len > TARGET)
                throw new Error("round " + round + ": length overshoot " + len);
            // I33 out-of-line clause: read a property that may live past the
            // loaded spine's outOfLineFragmentCount.
            const p = a["q" + ((s * 7) % PROPS)];
            if (p !== undefined && typeof p !== "number")
                throw new Error("round " + round + ": out-of-line stale-spine read " + describe(p)
                    + " (I33 out-of-line)");
        }
        return true;
    });

    const grower = new Thread(() => {
        while (Atomics.load(gate, "go") === 0)
            sleepMs(1);
        for (let i = 1; i < TARGET; ++i) {
            a[i] = i * 3; // dense pushes: repeated T2 spine growth
            if (i % PROPS === 0) {
                a["q" + (i / PROPS - 1 + 0)] = i; // out-of-line spine growth interleaved
                sleepMs(1);
            }
        }
        for (let p = 0; p < PROPS; ++p)
            a["q" + p] = p * 5; // final deterministic values
        return true;
    });

    Atomics.store(gate, "go", 1);
    shouldBeTrue(reader.join());
    shouldBeTrue(grower.join());

    shouldBe(a.length, TARGET, "round " + round);
    for (let i = 1; i < TARGET; ++i)
        shouldBe(a[i], i * 3, "round " + round + ": element lost under stale readers");
    for (let p = 0; p < PROPS; ++p)
        shouldBe(a["q" + p], p * 5, "round " + round + ": out-of-line property lost");
    shouldBe(a.seed, "segmented");
}
