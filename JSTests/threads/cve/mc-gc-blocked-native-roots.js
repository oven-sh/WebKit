//@ requireOptions("--useJSThreads=1", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1")
// MC-GC S1 (docs/threads/cve/map-MC-GC.md): premature reclaim under a
// blocked native frame — the CVE-2023-21954 analog. GIL-off, a spawned
// thread that parks in a blocking host primitive (property-path
// Atomics.wait) RELEASES its client's heap access (heap §10A / the §F.4
// spawned DAL bracket), so a conducted shared collection runs to completion
// while this thread is NoAccess. The collector's liveness view of the
// parked thread is exactly: (a) its registered machine stack + register
// snapshot (SPEC-heap I12, suspend-and-copy in Heap::gatherStackRoots), and
// (b) nothing else. Cells whose ONLY references live in the parked thread's
// JS/native frames must survive every collection conducted during the park
// — registration is I4(b)-permanent (Heap.cpp
// ensureCurrentThreadIsRegisteredForConservativeScan), and the scan covers
// NoAccess threads, not just access holders.
//
// Susceptibility oracle: after the parked thread wakes, every cell of a
// graph reachable ONLY from its locals still carries the exact values
// written before the park. A reclaimed-and-reused cell shows up as a wrong
// property value, a type confusion at the read, or a crash. Any of those =
// I12/§10A violation (collector reclaimed under a live native frame).
//
// EXECUTED POST-UNGIL ONLY (do not run against the mid-bring-up tree).
// Deterministic: the rendezvous guarantees the GC storm runs strictly
// inside the park window. Also meaningful (weaker) under the phase-1 GIL,
// where the property-path wait drops the GIL instead.
load("../harness.js", "caller relative");

const CELLS = 3000;
const GC_ROUNDS = 12;
const gate = { parked: 0, go: 0, gcsDone: 0 };

const t = new Thread(gate => {
    // Build a graph whose only roots are this frame's locals. Mix shapes so
    // a premature reclaim corrupts something checkable: plain objects,
    // strings built at runtime (not atoms baked into the code), arrays, and
    // a linked chain (so one lost cell breaks the walk, not just one slot).
    let head = null;
    const ring = [];
    for (let i = 0; i < CELLS; ++i) {
        const node = {
            index: i,
            tag: "node-" + i + "-" + (i * 7 + 13),
            box: [i, i + 1, i * 2],
            next: head,
        };
        head = node;
        if ((i % 5) === 0)
            ring.push(node);
    }

    // Rendezvous: signal we are about to park, then block. The wait is the
    // RHA-bracketed blocking primitive; the main thread runs the GC storm
    // strictly while we are NoAccess and only resumes us afterwards.
    Atomics.add(gate, "parked", 1);
    while (Atomics.load(gate, "go") === 0)
        Atomics.wait(gate, "go", 0);

    // The storm completed while we were parked (asserted by the counter the
    // main thread wrote BEFORE notifying). Now walk and verify everything.
    if (Atomics.load(gate, "gcsDone") < GC_ROUNDS)
        return "protocol-error: woke before the GC storm finished";
    let walked = 0;
    for (let node = head; node !== null; node = node.next) {
        const i = node.index;
        if (typeof i !== "number")
            return "corrupt: index not a number at walk position " + walked;
        if (node.tag !== "node-" + i + "-" + (i * 7 + 13))
            return "corrupt: tag mismatch at node " + i + " (" + node.tag + ")";
        if (node.box[0] !== i || node.box[1] !== i + 1 || node.box[2] !== i * 2)
            return "corrupt: box mismatch at node " + i;
        walked++;
    }
    if (walked !== CELLS)
        return "corrupt: chain length " + walked + " (expected " + CELLS + ")";
    for (const node of ring) {
        if (node.box[2] !== node.index * 2)
            return "corrupt: ring node " + node.index;
    }
    return "ok";
}, gate);

// Wait until the spawned thread has signalled imminent park, then give the
// park itself a moment to land (the signal precedes the wait by a few
// instructions; the storm below is long enough that the exact overlap point
// does not matter for soundness — every gc() after the park exercises the
// window, and at least the later rounds are guaranteed inside it).
waitUntil(() => Atomics.load(gate, "parked") === 1);
sleepMs(50);

// GC storm: full synchronous collections interleaved with allocation churn,
// so swept blocks are immediately reused — a premature reclaim of the parked
// thread's graph gets OVERWRITTEN, not just unmapped, making corruption
// observable at wake rather than silently surviving in free memory.
for (let r = 0; r < GC_ROUNDS; ++r) {
    let churn = [];
    for (let i = 0; i < 8000; ++i)
        churn.push({ filler: i, s: "churn-" + r + "-" + i, a: [r, i] });
    gc();
    churn = null;
    Atomics.add(gate, "gcsDone", 1);
}

Atomics.store(gate, "go", 1);
Atomics.notify(gate, "go");

shouldBe(t.join(), "ok");
