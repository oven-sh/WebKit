//@ requireOptions("--useJSThreads=1")
// SPEC-objectmodel I28 suite: shared-Double races (§4.7 R-DOUBLE).
//
// ContiguousDouble payloads are RAW doubles; a racing in-place relabel is
// type confusion, and a torn 8-byte slot is a garbage double. R-DOUBLE:
// shared Double STAYS Double — conversion aliases/builds raw-double
// fragments (aligned 8B slots, tear-free SAB semantics, holes = PNaN), no
// reboxing. Shape changes TOUCHING Double on an SW=1/segmented object
// (Int32<->Double, Double->Contiguous) are per-event STW relabels (I28): no
// reader interprets a slot under a shape not M7-ordered/re-checked.
// Observable: racing double writes/reads only ever yield values from the
// written set or undefined (hole); crossing Double->Contiguous mid-race
// never yields a "double reinterpretation" of a pointer or vice versa.
load("../harness.js", "caller relative");

const ROUNDS = 6;
const LEN = 128;
const SAMPLES = 600;

// (a) Foreign writes into a shared Double array racing readers: values are
// from {initial, written} only; growth keeps Double shape (R-DOUBLE: no
// sharing-onset STW, no boxing).
for (let round = 0; round < ROUNDS; ++round) {
    const d = new Array(LEN);
    for (let i = 0; i < LEN; ++i)
        d[i] = i + 0.5; // ContiguousDouble
    const gate = { go: 0 };

    const writer = new Thread(() => {
        while (Atomics.load(gate, "go") === 0)
            sleepMs(1);
        for (let i = 0; i < LEN; ++i) {
            d[i] = i + 0.25; // foreign double store: SW flip then raw 8B stores
            if (!(i & 31))
                sleepMs(1);
        }
        // Foreign growth of a shared Double array: stays Double (§4.7).
        for (let i = LEN; i < LEN * 2; ++i)
            d[i] = i + 0.125;
        return true;
    });
    const reader = new Thread(() => {
        while (Atomics.load(gate, "go") === 0)
            sleepMs(1);
        for (let s = 0; s < SAMPLES; ++s) {
            const i = (s * 31) % (LEN * 2);
            const v = d[i];
            if (v !== undefined && v !== i + 0.5 && v !== i + 0.25 && v !== i + 0.125)
                throw new Error("round " + round + ": shared-Double read tore: d[" + i + "] = "
                    + describe(v) + " (I28/§4.7)");
        }
        return true;
    });

    Atomics.store(gate, "go", 1);
    shouldBeTrue(writer.join());
    shouldBeTrue(reader.join());

    shouldBe(d.length, LEN * 2, "round " + round);
    for (let i = 0; i < LEN; ++i)
        shouldBe(d[i], i + 0.25, "round " + round + ": foreign double store lost");
    for (let i = LEN; i < LEN * 2; ++i)
        shouldBe(d[i], i + 0.125, "round " + round + ": shared-Double growth lost");
}

// (b) Double -> Contiguous relabel under shared access (per-event STW, I28):
// a foreign thread stores a NON-double into the shared Double array while
// readers stream it. Reads must be a written double, the stored object, or
// undefined — never a number that "is" the object's pointer bits, never a
// crash dereferencing double bits as a cell.
for (let round = 0; round < ROUNDS; ++round) {
    const d = [];
    for (let i = 0; i < LEN; ++i)
        d[i] = i * 1.5;
    new Thread(() => { d[0] = 0 * 1.5; }).join(); // establish SW=1 while still Double
    const marker = { mark: "relabeled" };
    const gate = { go: 0 };

    const relabeler = new Thread(() => {
        while (Atomics.load(gate, "go") === 0)
            sleepMs(1);
        sleepMs(2);
        d[7] = marker; // Double -> Contiguous on a shared object: per-event STW relabel
        return true;
    });
    const reader = new Thread(() => {
        while (Atomics.load(gate, "go") === 0)
            sleepMs(1);
        for (let s = 0; s < SAMPLES; ++s) {
            const i = (s * 17) % LEN;
            const v = d[i];
            if (i === 7) {
                if (v !== 7 * 1.5 && v !== marker)
                    throw new Error("round " + round + ": relabel race read " + describe(v) + " (I28)");
            } else if (v !== i * 1.5)
                throw new Error("round " + round + ": unrelated slot perturbed by relabel: d[" + i + "] = "
                    + describe(v) + " (I28)");
            if (!(s & 127))
                sleepMs(1);
        }
        return true;
    });

    Atomics.store(gate, "go", 1);
    shouldBeTrue(relabeler.join());
    shouldBeTrue(reader.join());

    shouldBe(d[7], marker, "round " + round + ": relabeling store lost");
    for (let i = 0; i < LEN; ++i) {
        if (i !== 7)
            shouldBe(d[i], i * 1.5, "round " + round + ": double corrupted by relabel");
    }
}

// (c) Int32 -> Double on a shared array (also a Double-touching shape change,
// I28): foreign thread stores a fractional value into a shared Int32 array.
for (let round = 0; round < ROUNDS; ++round) {
    const a = [];
    for (let i = 0; i < 64; ++i)
        a[i] = i; // Int32
    new Thread(() => { a[1] = 1; }).join(); // SW=1, still Int32
    new Thread(() => { a[5] = 5.5; }).join(); // Int32 -> Double under shared access
    shouldBe(a[5], 5.5, "round " + round);
    for (let i = 0; i < 64; ++i) {
        if (i !== 5)
            shouldBe(a[i], i, "round " + round + ": Int32->Double relabel corrupted a[" + i + "]");
    }
}
