//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// SPEC-objectmodel §4.4 T4-O (r17): relabels of a typed indexing shape
// (Int32->Double, Int32->Contiguous, Double->Contiguous) on an array the
// calling thread owns no longer stop the world. GIL on all three run in place;
// GIL off Int32->Contiguous does, an Int32->Double request is served as
// Int32->Contiguous, and Double->Contiguous keeps its stop - but since r18
// (T4-C) the `slice()` of a Double source used here is Contiguous GIL off, so
// there is nothing left to relabel on that leg either. Checks: (1) stop
// requests stay near zero across thousands of owned relabels with a second
// thread running (before: one stop per relabel); (2) GIL off an Int32 array
// that takes a double ends up Contiguous, not Double; (3) a foreign thread
// reading, slicing and concatenating arrays while their owner relabels them
// and stores strings into them sees only ints or the strings, its copies hold
// only those, and a full collection of everything it built stays sound.
load("../harness.js", "caller relative");

const gilOn = $vm.useThreadGIL();
const shapeOf = a => $vm.indexingMode(a);

// A second thread must be running for a stop to be requested at all.
const spin = { stop: 0 };
const spinner = new Thread(() => { let n = 0; while (!Atomics.load(spin, "stop")) n++; return n; });

// Sources whose shape does not adapt through an allocation profile (a literal
// or `new Array` site soon allocates pre-converted arrays and stops
// relabelling): slice() results are typed from their source, JSON.parse types
// by content. This is also the shape of the code that motivated the change
// (copies of Int32 arrays that then take one double).
const int32Base = JSON.parse("[1,2,3,4]");
const doubleBase = JSON.parse("[1.5,2.5,3.5]");
function ownedRelabels(kind, n) {
    let sum = 0;
    for (let i = 0; i < n; ++i) {
        if (kind === "i2d") { const a = int32Base.slice(); a[1] = 2.5; sum += a[1]; }
        else if (kind === "i2c") { const a = int32Base.slice(); a[1] = "s"; sum += a[1].length; }
        else { const d = doubleBase.slice(); d[1] = "t"; sum += d[1].length + d[0]; } // Double -> Contiguous
    }
    return sum;
}
for (const kind of ["i2d", "i2c", "d2c"]) ownedRelabels(kind, 200); // warm (first-use fires, tier-up)

const results = {};
for (const kind of ["i2d", "i2c", "d2c"]) {
    const before = $vm.jsThreadsStopRequestCount();
    const sum = ownedRelabels(kind, 4000);
    results[kind] = { stops: $vm.jsThreadsStopRequestCount() - before, sum };
}
Atomics.store(spin, "stop", 1);
spinner.join();

if (results.i2d.sum !== 4000 * 2.5) throw new Error("i2d sum " + results.i2d.sum);
if (results.i2c.sum !== 4000) throw new Error("i2c sum " + results.i2c.sum);
if (results.d2c.sum !== 4000 * 2.5) throw new Error("d2c sum " + results.d2c.sum);
// A few stops may come from elsewhere (collections, first-use fires).
if (results.i2d.stops > 100) throw new Error("owned Int32->Double relabels requested " + results.i2d.stops + " stops");
if (results.i2c.stops > 100) throw new Error("owned Int32->Contiguous relabels requested " + results.i2c.stops + " stops");
if (results.d2c.stops > 100) throw new Error("owned Double->Contiguous relabels requested " + results.d2c.stops + " stops (GIL off the slice of a Double source is Contiguous since r18, so no relabel happens)");

// (2) Shapes.
{
    const a = int32Base.slice(); a[1] = 0.5;
    const s = shapeOf(a);
    if (gilOn && !/Double/.test(s)) throw new Error("GIL on: Int32 + double should be Double, got " + s);
    if (!gilOn && !/Contiguous/.test(s)) throw new Error("GIL off: Int32 + double should become Contiguous, got " + s);
    if (a[1] !== 0.5 || a[0] + a[2] !== 4) throw new Error("values after relabel: " + a);
    const u = new Array(3); u[0] = 1.5; // Undecided -> Double stays Double in both modes
    if (!/Double/.test(shapeOf(u))) throw new Error("Undecided + double should be Double, got " + shapeOf(u));
    const c = doubleBase.slice(); // r18 T4-C: a fresh copy of a Double source is Contiguous GIL off, Double GIL on
    if (gilOn && !/Double/.test(shapeOf(c))) throw new Error("GIL on: slice of Double should be Double, got " + shapeOf(c));
    if (!gilOn && !/Contiguous/.test(shapeOf(c))) throw new Error("GIL off: slice of Double should be Contiguous, got " + shapeOf(c));
    if (c[0] !== 1.5 || c.length !== 3) throw new Error("slice values " + c);
}

// (3) Foreign reader / copier while the owner relabels Int32 arrays and stores strings.
const box = { stop: 0, slot: null, bad: "", copies: 0 };
const reader = new Thread(() => {
    const keep = [];
    let seen = 0;
    function check(v, where) {
        if (v === undefined) return;
        const t = typeof v;
        if (t === "number") { if ((v | 0) !== v) box.bad ||= where + " saw non-int number " + v; return; }
        if (t === "string") { if (v !== "str") box.bad ||= where + " saw string " + v; return; }
        box.bad ||= where + " saw " + t + " " + String(v);
    }
    while (!Atomics.load(box, "stop") && !box.bad) {
        const a = box.slot;
        if (!a) continue;
        for (let i = 0; i < a.length; ++i) check(a[i], "read"), ++seen;
        let s = 0;
        for (let i = 0; i < a.length; ++i) s += a[i] | 0; // Int32-mode loads in the optimizing tiers
        const c1 = a.slice(0);
        const c2 = a.concat([7]);
        const c3 = [5].concat(a);
        const c4 = Array.from(a);
        for (const c of [c1, c2, c3, c4]) for (let i = 0; i < c.length; ++i) check(c[i], "copy");
        keep.push(c1, c2, c3, c4);
        if (keep.length > 4000) { keep.length = 0; fullGC(); } // mark every copy: a mislabelled Int32 copy holding a cell must not be a hole
        box.copies += 4;
    }
    fullGC();
    return seen;
});
const int32Base8 = JSON.parse("[9,1,2,3,4,5,6,7]");
for (let i = 0; i < 30000 && !box.bad; ++i) {
    const a = int32Base8.slice(); // owned, Int32
    a[0] = i & 1023;
    box.slot = a;        // publish while Int32
    a[3] = "str";        // Int32 -> Contiguous (stop-free for the owner), then a cell in a former Int32 lane
    a[5] = "str";
    if ((i & 1023) === 0) gc();
}
Atomics.store(box, "stop", 1);
reader.join();
if (box.bad) throw new Error(box.bad);
print("PASS");
