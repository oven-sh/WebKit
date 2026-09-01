//@ requireOptions("--useJSThreads=1", "--useThreadGILOffUnsafe=1", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--thresholdForJITAfterWarmUp=20", "--thresholdForOptimizeAfterWarmUp=100")
// MC-SAFE S4 (docs/threads/cve/map-MC-SAFE.md): GC-completion waits vs a
// §A.3 thread-granular Class-A stop — the GCL-ordering shield.
//
// GC-completion waits (Heap::waitForCollector, Heap.cpp:2497-2532) park on
// ParkingLot::compareAndPark while HOLDING heap access and poll neither the
// §A.3 stop word nor the lite stop bits. The §A.3.2 conductor predicate is
// access-based, so if a stop word could be pending while a sibling sits in
// such a wait, the predicate would never converge and the 30s stop watchdog
// (JSThreadsSafepoint.cpp:401-413) would fail-stop the process. The tree's
// claimed shield is ORDER, not polling: the conductor takes
// Heap::JSThreadsStopScope (the rank-2 GC conductor lock) BEFORE publishing
// the stop word (HBT4.5, VMManager.cpp:560-570) and queues behind any
// in-progress shared GC (§10C(b)/(e)) — so no §A.3 window can open while a
// collection that someone is waiting on is mid-cycle. Note the unwired
// FIX-2 helper (JSThreadsSafepoint::parkSitePollAndParkForStopTheWorld has
// ZERO call sites) names "GC-completion waits" as a caller it never got:
// this test is the empirical check that the ordering shield alone holds.
//
// Shape: sibling threads run a synchronous-GC storm (each gc() call ends in
// a GC-completion wait) plus allocation pressure; the main thread runs a
// Class-A jettison storm. Every stop must converge well under the 30s
// watchdog; a hole in the ordering shield shows up as a watchdog crash.
//
// EXECUTED POST-UNGIL ONLY. Deterministic pass criterion; amplifier-ready
// (the race window is the gap between a sibling's GC request and the
// conductor's GCL acquisition — more rounds widen exposure).
load("../harness.js", "caller relative");

const GC_THREADS = 2;
const ROUNDS = 12;
const gate = { started: 0, stop: 0 };

const gcers = spawnN(GC_THREADS, () => {
    Atomics.add(gate, "started", 1);
    let cycles = 0;
    let churn = null;
    while (Atomics.load(gate, "stop") === 0) {
        // Allocation pressure so collections have real work, then a
        // synchronous full GC: the caller ends up in a GC-completion wait
        // for its ticket.
        churn = new Array(4096).fill(cycles);
        gc();
        ++cycles;
    }
    return cycles + (churn ? 1 : 0);
});

waitUntil(() => Atomics.load(gate, "started") === GC_THREADS);

const nowMs = (typeof preciseTime === "function") ? () => preciseTime() * 1000 : () => Date.now();

function buildVictim(round) {
    const proto = { y: 1 };
    const o = Object.create(proto);
    const f = Function("o", "/* gcwait round " + round + " */ return o.y + 1;");
    for (let i = 0; i < 2000; ++i)
        f(o);
    return { proto, o, f };
}

for (let r = 0; r < ROUNDS; ++r) {
    const { proto, o, f } = buildVictim(r);
    const t0 = nowMs();
    proto.y = 2 + r; // Class-A fire => jettison => §A.3 stop, racing the GC storm.
    const ms = nowMs() - t0;
    shouldBe(f(o), 3 + r);
    shouldBeTrue(ms < 20000, "round " + r + " stop converged against GC-completion waiters (took " + ms + "ms)");
}

Atomics.store(gate, "stop", 1);
const counts = joinAll(gcers);
for (const c of counts)
    shouldBeTrue(c > 0, "GC thread made progress");
