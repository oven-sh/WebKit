//@ requireOptions("--useJSThreads=1")
// SPEC-ungil ANNEX C1 / U-T10 (U28-class CAS storm): lock-free-arm exactness.
//
// N threads CAS-increment counters living in every lock-free §9.5 arm:
//   - an INLINE named slot,
//   - an OUT-OF-LINE named slot (pushed out of line by 200 prior properties;
//     foreign out-of-line adds may also drive the receiver segmented, which
//     exercises the fragment-slot CAS arm),
//   - a CONTIGUOUS indexed element born Int32 (the first atomic access must
//     CONVERT to Contiguous - raw-word CAS on Int32/Double is rejected, 8g),
//   - a Double-born indexed element (same conversion rule),
// plus an Atomics.add RMW storm on a named slot. Under the GIL this is the
// trivially-serialized oracle (U19); GIL-off a single lost update breaks the
// exact final counts.
load("../harness.js", "caller relative");

const THREADS = 4;
const PER = 1200;

const inlineObj = { n: 0 };

const oolObj = {};
for (let i = 0; i < 200; ++i)
    oolObj["p" + i] = i;
oolObj.n = 0;

const int32Arr = [0, 0, 0, 0]; // Int32 shape until the first atomic access.
const doubleArr = [0.5, 0.5]; // Double shape until the first atomic access.
doubleArr[0] = 0; // value 0, shape stays Double (0 stored as double)

const rmwObj = { m: 0 };

function casIncrementLoop(o, k, count) {
    for (let i = 0; i < count; ++i) {
        for (;;) {
            const cur = Atomics.load(o, k);
            if (Atomics.compareExchange(o, k, cur, cur + 1) === cur)
                break;
        }
    }
}

const threads = [];
for (let t = 0; t < THREADS; ++t) {
    threads.push(new Thread(() => {
        casIncrementLoop(inlineObj, "n", PER);
        casIncrementLoop(oolObj, "n", PER);
        casIncrementLoop(int32Arr, "0", PER);
        casIncrementLoop(doubleArr, "0", PER);
        for (let i = 0; i < PER; ++i)
            Atomics.add(rmwObj, "m", 1);
        return true;
    }));
}
for (const t of threads)
    shouldBeTrue(t.join());

shouldBe(inlineObj.n, THREADS * PER, "inline-slot CAS increments are exact");
shouldBe(oolObj.n, THREADS * PER, "out-of-line-slot CAS increments are exact");
shouldBe(int32Arr[0], THREADS * PER, "indexed CAS increments are exact (Int32 converts on first atomic access)");
shouldBe(doubleArr[0], THREADS * PER, "indexed CAS increments are exact (Double converts on first atomic access)");
shouldBe(rmwObj.m, THREADS * PER, "Atomics.add RMW storm is exact");

// SVZ rope arm under contention: expected values built as ropes must still
// match by value (resolution happens outside any lock and the probe
// restarts).
const ropeObj = { s: "left" + "right" };
const ropeThreads = [];
for (let t = 0; t < 2; ++t) {
    ropeThreads.push(new Thread(() => {
        let swaps = 0;
        for (let i = 0; i < 400; ++i) {
            if (Atomics.compareExchange(ropeObj, "s", "left" + "right", "le" + "ftright") === "leftright")
                ++swaps;
            if (Atomics.compareExchange(ropeObj, "s", "leftri" + "ght", "left" + "right") === "leftright")
                ++swaps;
        }
        return swaps >= 0;
    }));
}
for (const t of ropeThreads)
    shouldBeTrue(t.join());
shouldBe(ropeObj.s, "leftright", "rope-expected CAS converges to a value-equal string");
