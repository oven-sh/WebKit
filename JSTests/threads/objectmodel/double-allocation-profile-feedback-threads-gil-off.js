//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--countJSThreadsCounters=1")
// SPEC-objectmodel T4-P as amended in the ninth round (history §32): GIL off,
// optimized allocations from a Double-recommending profile report their array
// to the profile's last-array word only when the word holds none or for about
// one allocation in 32 per thread, so that threads allocating from one site
// stop rewriting a shared cache line on every allocation. The site must still
// notice that its arrays are being converted and stop minting Double arrays -
// here with four threads allocating from the same literal site at once, each
// converting what it allocates. GIL on / flag off: nothing to converge, values
// checked only.
load("../harness.js", "caller relative");

if (typeof Thread !== "function") {
    print("SKIP: needs jsc shell with Thread");
    quit(0);
}

function alloc(x) { return [x + 0.5, x * 0.25, 3.75, -1.5]; } // op_new_array, profile learns Double
noInline(alloc);
function sumDouble(a) { return a[0] + a[1] + a[2] + a[3]; } // Double-only consumer
noInline(sumDouble);
function sumMixed(a) { let s = 0; for (let i = 0; i < a.length; ++i) s += a[i]; return s; } // also sees Contiguous arrays
noInline(sumMixed);
function convert(a) { a[1] = "x"; return a; } // Double -> Contiguous: a stop GIL off
noInline(convert);

const THREADS = 4;
const WARM = 150000;
const N = 40000;

function phaseA() { let c = 0; for (let i = 0; i < WARM; ++i) c += sumDouble(alloc(i)); return c; }
function phaseB(count) {
    let s = 0;
    for (let i = 0; i < count; ++i) {
        const a = convert(alloc(i));
        s += sumMixed([a[0], a[2], a[3]]);
    }
    return s;
}
function expectedB(count) {
    let e = 0;
    for (let i = 0; i < count; ++i) e += (i + 0.5) + 3.75 - 1.5;
    return e;
}
function onThreads(f) {
    const threads = [];
    for (let t = 1; t < THREADS; ++t)
        threads.push(new Thread(f));
    const results = [f()];
    for (const t of threads)
        results.push(t.join());
    return results;
}

// Phase A: every thread tiers alloc() up with a Double recommendation.
const warm = onThreads(phaseA);
for (const w of warm) {
    if (w !== warm[0])
        throw new Error("phase A results differ: " + warm);
}

// Phase B: every thread's arrays are converted after allocation. The first three quarters let the profile learn;
// the last quarter must see (almost) no conversions of freshly allocated Double arrays.
const counter = () => $vm.jsThreadsCounter("OM relabel Double->Contiguous");
const stops0 = counter();
const early = onThreads(() => phaseB((N * 3) / 4));
const late0 = counter();
const late = onThreads(() => phaseB(N / 4));
const lateStops = counter() - late0;

const e1 = expectedB((N * 3) / 4), e2 = expectedB(N / 4);
for (const r of early) {
    if (Math.abs(r - e1) > 1e-3)
        throw new Error("phase B (early) result " + r + " expected " + e1);
}
for (const r of late) {
    if (Math.abs(r - e2) > 1e-3)
        throw new Error("phase B (late) result " + r + " expected " + e2);
}
if (typeof AMPLIFY_VERBOSE !== "undefined")
    print("Double->Contiguous stops: " + (counter() - stops0) + " of " + (THREADS * N) + " arrays; in the last quarter: " + lateStops);
if (lateStops > 2000)
    throw new Error("the allocation site still mints Double arrays that are converted on arrival: " + lateStops + " stops in the last " + (THREADS * N / 4) + " allocations");
print("PASS");
