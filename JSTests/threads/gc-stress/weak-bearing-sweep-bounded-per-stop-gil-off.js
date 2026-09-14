//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--useGenerationalGC=false")
// SPEC-heap history §35. GIL off, the end of every conducted cycle sweeps the
// unswept weak-bearing blocks (blocks a Weak<> points into; AUDIT R9-18). A
// Full collection makes every block unswept again, so back-to-back Full cycles
// re-swept all of them each time: the stress suite's continuous-collection
// lanes swept about 800 blocks at each of about 20,000 cycle ends a minute.
// One unrequested cycle end now sweeps at most the budget (32 by default),
// resuming where the last one stopped; a requested collection still sweeps
// them all. Here: tens of thousands of structures, each named weakly by its
// parent's transition table, and allocation-paced Full cycles.

if (typeof $vm === "undefined" || typeof $vm.weakBearingSweepStats !== "function") {
    print("SKIP: needs $vm.weakBearingSweepStats");
    quit(0);
}

const BUDGET = 32;

const keep = [];
for (let i = 0; i < 60000; ++i) {
    const o = {};
    o["p" + (i % 1000)] = i;
    o["q" + i] = i;
    keep.push(o);
}

let sink = 0;
for (let round = 0; round < 200; ++round) {
    let junk = [];
    for (let i = 0; i < 20000; ++i)
        junk.push({ i, round });
    sink += junk.length;
}

const [cycles, blocks, maxUnrequested] = $vm.weakBearingSweepStats();
if (maxUnrequested > BUDGET)
    throw new Error(`one unrequested cycle end swept ${maxUnrequested} weak-bearing blocks (budget ${BUDGET}); ${blocks} blocks in ${cycles} cycle ends`);
// GIL off the heap is shared and the sweep must still run. (Flag off the shell has no Thread, and useThreadGIL()
// reads false there too.)
if (typeof Thread === "function" && !$vm.useThreadGIL() && !cycles)
    throw new Error("GIL off, no cycle end swept a weak-bearing block");
if (keep.length !== 60000 || sink !== 200 * 20000)
    throw new Error("workload did not run");
print("PASS");
