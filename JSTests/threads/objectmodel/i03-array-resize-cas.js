//@ requireOptions("--useJSThreads=1")
// SPEC-objectmodel I16/I10 suite: element-storage resizes are
// butterfly-pointer CASes; foreign resizes go segmented under STW-fired
// watchpoints.
//
// I16: element-storage resizes never touch the header — they CAS ONLY the
// tagged butterfly word (§4.4). I10: a FOREIGN butterfly transition
// (element resizes included) fires both TTL sets under a stop and produces a
// segmented object (sole exception AS, I31). I17: every butterfly-pointer
// mutation on an object with indexed properties is (D)CAS even under the
// cell lock. Observable program-level consequences raced here:
//   - push storms from a foreign thread grow correctly (every resize CAS
//     loser re-dispatches, never blind-retries onto a stale word);
//   - a concurrent reader sees length grow monotonically and every readable
//     element holds its writer's value;
//   - named properties on the same object survive the element resizes (a
//     header-touching "resize" would race the property transitions).
load("../harness.js", "caller relative");

const ROUNDS = 6;
const TARGET = 1500; // crosses many vectorLength steps => many resize CASes
const SAMPLES = 600;

for (let round = 0; round < ROUNDS; ++round) {
    const a = [0]; // main-thread-installed: every grower below is FOREIGN (I10)
    a.named = "before-resizes";
    const gate = { go: 0 };

    const grower = new Thread(() => {
        while (Atomics.load(gate, "go") === 0)
            sleepMs(1);
        for (let i = 1; i < TARGET; ++i) {
            a.push(i); // foreign pushes: repeated resize CASes (T2/segmented)
            if (!(i & 255))
                sleepMs(1);
        }
        return true;
    });
    const reader = new Thread(() => {
        while (Atomics.load(gate, "go") === 0)
            sleepMs(1);
        let lastLen = 0;
        for (let s = 0; s < SAMPLES; ++s) {
            const len = a.length;
            if (len < lastLen)
                throw new Error("round " + round + ": length went backwards " + lastLen + " -> " + len + " (I16)");
            lastLen = len;
            if (len > 0) {
                const i = (s * 37) % len;
                const v = a[i];
                if (v !== undefined && v !== i)
                    throw new Error("round " + round + ": resize lost/aliased a[" + i + "] = " + describe(v) + " (I16/I17)");
            }
            const named = a.named;
            if (named !== "before-resizes" && named !== "after-resizes")
                throw new Error("round " + round + ": named property torn by resize: " + describe(named));
        }
        return true;
    });

    Atomics.store(gate, "go", 1);
    shouldBeTrue(grower.join());
    a.named = "after-resizes"; // owner write AFTER foreign growth (now shared/segmented)
    shouldBeTrue(reader.join());

    shouldBe(a.length, TARGET, "round " + round + ": every push landed exactly once");
    for (let i = 0; i < TARGET; ++i)
        shouldBe(a[i], i, "round " + round + ": element lost in resize CAS storm");
    shouldBe(a.named, "after-resizes");
}

// Two foreign growers, disjoint halves, racing each other's resize CASes on
// the SAME array: the loser of each CAS must re-dispatch onto the winner's
// (possibly segmented) butterfly — both halves must land in full.
for (let round = 0; round < ROUNDS; ++round) {
    const N = 600;
    const a = [];
    a[N - 1] = -1; // pre-size the public length; both halves still grow storage
    const w0 = new Thread(() => {
        for (let i = 0; i < N / 2; ++i)
            a[i] = i + 0; // low half
        return true;
    });
    const w1 = new Thread(() => {
        for (let i = N - 2; i >= N / 2; --i)
            a[i] = i + 0; // high half, downwards: maximally disjoint CAS pattern
        return true;
    });
    shouldBeTrue(w0.join());
    shouldBeTrue(w1.join());

    shouldBe(a.length, N, "round " + round);
    for (let i = 0; i < N - 1; ++i)
        shouldBe(a[i], i, "round " + round + ": dual-grower element lost a[" + i + "]");
    shouldBe(a[N - 1], -1);
}
