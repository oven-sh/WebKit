//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--countJSThreadsCounters=1")
// SPEC-objectmodel T4-P (rev 18, history §27). An array-literal site that is
// already in optimized code with a Double allocation profile, whose arrays
// later start being converted to Contiguous at a use site, must stop minting
// Double arrays GIL off (each conversion is a stop-the-world there): the
// optimized allocation reports its arrays to the profile, the profile fires
// its demotion watchpoint when its recommendation leaves Double, and the site
// recompiles (the allocation also notices when the array it reported last was
// converted and updates the profile at once). Before: the conversions never
// stop (196,969 for 200,000 arrays here, 0.85 s in the conductor); after: 2.
// GIL on / flag off: nothing to converge, values checked only.
load("../harness.js", "caller relative");

function alloc(x) { return [x + 0.5, x * 0.25, 3.75, -1.5]; } // op_new_array, profile learns Double
noInline(alloc);
function sumDouble(a) { return a[0] + a[1] + a[2] + a[3]; } // Double-only consumer: keeps alloc's arrays Double while it tiers up
noInline(sumDouble);
function sumMixed(a) { let s = 0; for (let i = 0; i < a.length; ++i) s += a[i]; return s; } // later also sees Contiguous arrays
noInline(sumMixed);
function grown(x) { const a = [x | 0, 1, 2]; a[3] = x + 0.5; return a; } // Contiguous GIL off (Int32 grown by a double), Double GIL on
noInline(grown);

// Phase A: tier alloc() up with a Double recommendation.
let check = 0;
for (let i = 0; i < 200000; ++i) check += sumDouble(alloc(i));

// Phase B: the same arrays now reach a site that converts them.
const stops0 = $vm.jsThreadsCounter("OM relabel Double->Contiguous");
const N = 200000;
let total = 0, late = 0;
for (let i = 0; i < N; ++i) {
    total += sumMixed(alloc(i)) + sumMixed(grown(i));
    if (i === (N * 3) / 4) late = $vm.jsThreadsCounter("OM relabel Double->Contiguous");
}
const stops = $vm.jsThreadsCounter("OM relabel Double->Contiguous") - stops0;
const lateStops = $vm.jsThreadsCounter("OM relabel Double->Contiguous") - late; // conversions in the last quarter

let expected = 0;
for (let i = 0; i < N; ++i) expected += (i + 0.5 + i * 0.25 + 3.75 - 1.5) + ((i | 0) + 1 + 2 + i + 0.5);
if (Math.abs(total - expected) > 1e-3) throw new Error("total " + total + " expected " + expected);
if (typeof AMPLIFY_VERBOSE !== "undefined") print("Double->Contiguous stops: " + stops + " of " + N + " arrays; in the last quarter: " + lateStops + "; profile demotions: " + $vm.jsThreadsCounter("arrayAllocationProfileLeftDoubleGILOff"));
if (lateStops > 1000) throw new Error("allocation site still mints Double arrays that are converted on arrival: " + lateStops + " stops in the last " + (N / 4) + " (total " + stops + ")");

// A second thread runs the converged workload too (shared CodeBlocks/profiles).
if (typeof Thread === "function") {
    const t = new Thread(() => { let s = 0; for (let i = 0; i < 20000; ++i) s += sumMixed(alloc(i)) + sumMixed(grown(i)); return s; });
    let mineB = 0; for (let i = 0; i < 20000; ++i) mineB += sumMixed(alloc(i)) + sumMixed(grown(i));
    const theirs = t.join();
    if (Math.abs(theirs - mineB) > 1e-3) throw new Error("thread " + theirs + " vs " + mineB);
}
print("PASS");
