//@ requireOptions("--useSharedGCHeap=1", "--useDollarVM=1")
// SPEC-heap.md T10: I17 (per-client DeferGC once ISS) and I14 (STW-forbidden
// scopes).
//
// deferralVsAllocationStorm: each client brackets allocation bursts in its
// OWN deferral depth (routed via the §10A.1 TLS stamp) while another client
// keeps collecting — one client's deferral never defers another client's
// collection, one client's decrement never closes another's scope, and a
// deferred client still parks for a pending stop (SINFAC's GSP handling
// precedes its isDeferred() conduction skip).
//
// structureLockVsSTW: the I14 shape — inside an STW-forbidden scope a thread
// neither initiates nor joins a stop; its allocations run deferred (the L5
// GCDeferralContext discipline), and the I14 debug counters at the
// CSAC/RCAC/SINFAC/election entries assert it.
//
// Runnable in the no-JIT TSAN config and JIT-on (§5.5).
load("./resources/assert.js", "caller relative");

if (typeof $vm !== "undefined" && typeof $vm.sharedHeapTest === "function") {
    shouldBeTrue($vm.sharedHeapTest("deferralVsAllocationStorm", 4, 2000), "deferralVsAllocationStorm");
    shouldBeTrue($vm.sharedHeapTest("structureLockVsSTW", 4, 500), "structureLockVsSTW");
}
print("PASS");
