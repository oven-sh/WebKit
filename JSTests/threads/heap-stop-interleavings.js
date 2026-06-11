//@ requireOptions("--useSharedGCHeap=1", "--useDollarVM=1")
// SPEC-heap.md T10: §10C — other stop reasons vs the shared-GC stop.
//
// This file is the "real VMs via $vm" half of §12.1: it drives the harness
// from the shell's REAL, ENTERED VM (the main client), so the conducted
// stops in these scenarios run with a VM-backed client in the picture —
// the manifest-5 VMManager hunks (GC keep-parked bit, park hooks, resume
// notify, re-latch) are live on this path in the overlay build.
//
//   debuggerStopDuringSharedGC (§10C(b)/(c)): a non-GC GCL stop
//     (JSThreadsStopScope — the same bracket a debugger/JSThreads conductor
//     holds) is requested while GC conductions are in flight; the bracket
//     waits for the conductor, and requesters arriving while it is held take
//     the §10.2 GCL-busy timed-wait path.
//   gcDuringDebuggerPark (§10C(a)/(e)): the bracket is HELD when sync GC
//     requesters arrive; they must park with timed (<=1ms) waits — never
//     spinning, never untimed — and complete after the bracket exits.
//   jsThreadsStopVsGCRequester (§10C(e)/G13): combined storm — bracket
//     churn + sync requesters + allocators; liveness and pattern integrity.
//
// Runnable in the no-JIT TSAN config and JIT-on (§5.5).
load("./resources/assert.js", "caller relative");

if (typeof $vm !== "undefined" && typeof $vm.sharedHeapTest === "function") {
    shouldBeTrue($vm.sharedHeapTest("debuggerStopDuringSharedGC", 4, 16), "debuggerStopDuringSharedGC");
    shouldBeTrue($vm.sharedHeapTest("gcDuringDebuggerPark", 4, 8), "gcDuringDebuggerPark");
    shouldBeTrue($vm.sharedHeapTest("jsThreadsStopVsGCRequester", 5, 16), "jsThreadsStopVsGCRequester");
}
print("PASS");
