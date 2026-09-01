//@ requireOptions("--useSharedGCHeap=1", "--useDollarVM=1")
// SPEC-congc CG-T2 (C0): runtime litmus companion to the U20 lock-order
// lint. THE STATIC AUTHORITY IS Tools/threads/lint-lockorder-u20.sh —
// written by CG-1 as the U20 extension to ANNEX CGS2.1's LK.9c/9d rows,
// encoding the three F21 clauses (CG-I10(1)-(3)) and the CGS2.2 composed
// chain (NL > GCL > m_markingMutex > CMS) plus the §3.4 disposition-marker
// check (every m_gcConductorLock.tryLock site classified, F47 watchdog row
// included). Its ADOPTION as the one lock-order authority is adoption gate
// §13.5(1) (OPEN at CG-1); the rev-7 "U20-class" private lint is retired —
// no second lock-order authority exists.
//
// This file is the RUNTIME arm: it drives the lock orders the chain walk
// covers that are reachable flag-off —
//  - GCL > m_markingMutex: a conducted cycle's parallel marking runs while
//    the conductor holds GCL (in-window; helpers take m_markingMutex);
//  - GCL handoffs vs foreign GCL holders (JSThreadsStopScope) under storm:
//    no inversion may wedge an election, a stop scope, or a poll (liveness
//    here is the litmus' pass criterion — a lock-order cycle deadlocks and
//    times out loudly).
// The CMS lock (LK.9c) does not exist until CG-2; its clauses are
// static-only rows in the lint until then.
load("./resources/assert.js", "caller relative");

if (typeof $vm !== "undefined" && typeof $vm.sharedHeapTest === "function") {
    // GCL-held marking (m_markingMutex inside GCL) under allocation load.
    shouldBeTrue($vm.sharedHeapTest("allocationStorm", 4, 12000), "allocationStorm");

    // Foreign GCL holders vs conductors vs allocators all at once: the
    // composed-chain edges reachable at C0, stormed.
    shouldBeTrue($vm.sharedHeapTest("jsThreadsStopVsGCRequester", 4, 24), "jsThreadsStopVsGCRequester");

    // Structure-lock (rank 8/SAL) holders vs stop initiation: I14/L5 — the
    // STW-forbidden-scope discipline the chain walk's "no 7-9b under
    // m_markingMutex" clause leans on.
    shouldBeTrue($vm.sharedHeapTest("structureLockVsSTW", 3, 16), "structureLockVsSTW");

    let sum = 0;
    for (let i = 0; i < 5000; ++i)
        sum += ({ v: i }).v;
    shouldBe(sum, 12497500);
}
print("PASS");
