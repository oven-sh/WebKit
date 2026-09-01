//@ requireOptions("--useJSThreads=1", "--useThreadGILOffUnsafe=1", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1")
// SIDE-FINDING repro extracted from mc-safe-gcwait-vs-classa-stop.js (MC-SAFE
// S4 audit, 2026-06-15): under a concurrent gc() storm from sibling threads,
// main-thread 3-fiber JSRopeString construction trips
//   ASSERTION FAILED: (s1->length() + s2->length() + s3->length()) == this->length()
//   JSString.h(694) : JSC::JSRopeString::JSRopeString(...)
// 5/5 on the Debug ASAN jsc. This is NOT the S4 mechanism (no 30s
// watchdogAssertStopProgress involvement); it is rope publish/length tearing
// under concurrent shared GC — see docs/threads/cve/map-MC-TEAR.md §S5 and
// docs/threads/TSAN-TRIAGE.md family 17 (rope-stringimpl) for the governing
// invariant. Filed here so the S4 test's deterministic crash has a minimal
// standalone repro alongside it.
load("../harness.js", "caller relative");

const gate = { started: 0, stop: 0 };

const gcers = spawnN(2, () => {
    Atomics.add(gate, "started", 1);
    let cycles = 0;
    let churn = null;
    while (Atomics.load(gate, "stop") === 0) {
        churn = new Array(4096).fill(cycles);
        gc();
        ++cycles;
    }
    return cycles + (churn ? 1 : 0);
});

waitUntil(() => Atomics.load(gate, "started") === 2);

let acc = 0;
for (let r = 0; r < 200; ++r) {
    // 3-fiber rope: literal + Int32->String + literal. The original S4 test
    // hits the assert on the very first such concat after the gc() storm
    // starts; loop to keep the window open if timing shifts.
    const src = "/* gcwait round " + r + " */ return o.y + 1;";
    acc += src.length;
}

Atomics.store(gate, "stop", 1);
joinAll(gcers);
if (acc === 0)
    throw new Error("unreachable");
