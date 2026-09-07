//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// SPEC-objectmodel T4 (r16): an Undecided -> Int32/Double/Contiguous relabel of
// an array the current thread owns is claim-first and stop-free, even after
// the Undecided shape's thread-local sets have fired somewhere in the process
// (F2 is per structure, so before this change every `new Array(n); a[0] = v`
// on every thread paid a stop-the-world from then on). Checks: (1) few stop
// requests for many owned relabels once the sets are fired; (2) a foreign
// thread reading arrays while their owner relabels them sees only holes
// (undefined) or the stored values.
load("../harness.js", "caller relative");

// Fire the Undecided shape's sets: a foreign thread converts an Undecided
// array this thread created (a shared relabel, which fires F2 on the shape).
{
    const seed = new Array(4);
    new Thread(() => { seed[0] = 1; }).join();
    if (seed[0] !== 1) throw new Error("seed store lost");
}

// (1) Owned relabels after the fire, with a second thread running (a stop
// is only expensive - and only counted as a request - when there is someone
// to stop). Array.from over an iterable and the mapped result are built by
// the generic path: an Undecided array filled by putDirectIndex, i.e. one
// relabel per array; `new Array(n)` at a hot site would soon be allocated
// pre-typed by its allocation profile and not exercise this.
const spin = { stop: 0 };
const spinner = new Thread(() => { let n = 0; while (!Atomics.load(spin, "stop")) n++; return n; });
const re = /(a)(b)?/;
const before = $vm.jsThreadsStopRequestCount();
let sum = 0;
for (let i = 0; i < 3000; ++i) {
    const m = re.exec("ac"); // ["a", "a", undefined]
    const a = Array.from(m).map(x => x === undefined ? 0.5 : x.length); // Undecided -> Contiguous, then -> Double/Int32 mixes
    sum += a.length + a[2];
    const d = Array.from({ length: 3 }); d[1] = i + 0.5; sum += d[1] - i; // Undecided -> Double
}
const stops = $vm.jsThreadsStopRequestCount() - before;
Atomics.store(spin, "stop", 1);
spinner.join();
if (sum !== 3000 * (3 + 0.5) + 3000 * 0.5) throw new Error("bad sum " + sum); // 12000
// A handful of stops may come from elsewhere (GC, first-use fires); thousands
// of owned relabels must not each request one (before: one stop per array).
if (stops > 200) throw new Error("owned Undecided relabels requested " + stops + " stops");

// (2) Foreign reader during owner relabels.
const box = { stop: 0, slot: null, bad: "" };
const reader = new Thread(() => {
    let seen = 0;
    while (!Atomics.load(box, "stop")) {
        const a = box.slot;
        if (!a) continue;
        for (let i = 0; i < 4; ++i) {
            const v = a[i];
            if (v !== undefined && v !== 7 && v !== 7.5 && v !== "x") { box.bad = "reader saw " + String(v); return seen; }
            ++seen;
        }
    }
    return seen;
});
for (let i = 0; i < 20000 && !box.bad; ++i) {
    const a = new Array(4);
    box.slot = a;                 // published while still Undecided
    switch (i % 3) {
    case 0: a[0] = 7; break;      // relabel under a possible concurrent reader
    case 1: a[1] = 7.5; break;
    case 2: a[2] = "x"; break;
    }
}
Atomics.store(box, "stop", 1);
reader.join();
if (box.bad) throw new Error(box.bad);
print("PASS");
