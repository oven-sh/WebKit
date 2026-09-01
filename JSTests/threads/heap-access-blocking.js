//@ requireOptions("--useSharedGCHeap=1", "--useDollarVM=1")
// SPEC-heap.md T10: the §10A/F8 access protocol under fire.
//
// blockedInNativeVsGC: clients bracket simulated blocking native calls with
// releaseHeapAccess/acquireHeapAccess while another client conducts. The
// re-acquire exercises the F8 Dekker pair: CAS to HasAccess, seq_cst GSP
// sample, mandatory revert + GBC block when a stop is pending. The race
// amplifier's AHA hook widens exactly that window.
//
// syncRequesterStorm: every client is a sync requester (§10.2 election:
// tryLock winners conduct and drain ALL granted tickets; losers release
// access and wait on the election condition).
//
// noEnteredVMsGC: the whole storm runs on standalone (VM-less) clients with
// the main VM's access released — the zero-entered-VMs stop path.
//
// Runnable in the no-JIT TSAN config and JIT-on (§5.5).
load("./resources/assert.js", "caller relative");

if (typeof $vm !== "undefined" && typeof $vm.sharedHeapTest === "function") {
    shouldBeTrue($vm.sharedHeapTest("blockedInNativeVsGC", 4, 2000), "blockedInNativeVsGC");
    shouldBeTrue($vm.sharedHeapTest("syncRequesterStorm", 4, 8), "syncRequesterStorm");
    shouldBeTrue($vm.sharedHeapTest("noEnteredVMsGC", 3, 8), "noEnteredVMsGC");
}
print("PASS");
