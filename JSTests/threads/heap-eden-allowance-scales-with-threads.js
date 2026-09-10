//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--countJSThreadsCounters=1")
// SPEC-heap §10F (eighth landing round). The shared heap's allocation trigger
// budgets one nursery; four threads allocating in parallel filled it four
// times as fast, and every eden collection stops all four, so the eden pause
// count per second of wall time grew with the thread count (raytrace-like at
// four threads: 63 edens and 47 ms of 275 in them). Now every additional
// client that is really allocating adds one capped nursery to the cycle's
// allowance. Measured here as the number of eden collections a fixed amount
// of allocation PER THREAD costs: one thread alone, then four threads each
// doing the same work. Before: about 4x the single-thread count (the total
// allocation is 4x against one budget); after: close to 1x GIL off. GIL on
// (one thread runs at a time, and the heap is not a shared server) the count
// legitimately stays about 4x, so only GIL off is asserted.
load("./harness.js", "caller relative");

new Thread(() => 1).join(); // the heap is shared from here on

function churn(rounds) {
    let keep = 0;
    for (let r = 0; r < rounds; ++r) {
        for (let c = 0; c < 60; ++c) {
            let a = [];
            for (let i = 0; i < 400; ++i) a.push({ i, r, t: [i, r] });
            keep += a[c & 7].i;
        }
    }
    return keep;
}
noInline(churn);

churn(60); // warm-up: tier-up and heap sizing
const ROUNDS = 150;
let e0 = $vm.jsThreadsCounter("gcEden") + $vm.jsThreadsCounter("gcFull");
churn(ROUNDS);
const single = $vm.jsThreadsCounter("gcEden") + $vm.jsThreadsCounter("gcFull") - e0;

e0 = $vm.jsThreadsCounter("gcEden") + $vm.jsThreadsCounter("gcFull");
const threads = []; for (let i = 0; i < 3; ++i) threads.push(new Thread(() => churn(ROUNDS)));
const mine = churn(ROUNDS);
for (const t of threads) if (t.join() !== mine) throw new Error("churn result differs across threads");
const four = $vm.jsThreadsCounter("gcEden") + $vm.jsThreadsCounter("gcFull") - e0;

if (typeof AMPLIFY_VERBOSE !== "undefined") print("collections for " + ROUNDS + " rounds: one thread " + single + ", four threads x the same work each " + four + " (ratio " + (four / Math.max(single, 1)).toFixed(2) + ")");
if (single < 4) throw new Error("workload too small to measure: " + single + " collections single-threaded");
if (!$vm.useThreadGIL() && four > single * 2.5) throw new Error("four allocating threads cost " + four + " collections against " + single + " for one (ratio " + (four / single).toFixed(2) + "); the eden allowance did not grow with the allocating clients");
print("PASS");
