//@ requireOptions("--useDollarVM=1")
// SPEC-heap.md T10: I11 epoch unit test (T7), driven from JS.
//
// epochReclaim MUST run in the 1-client !ISS configuration (the harness
// refuses otherwise), so this file runs it alone — no other heap-*.js
// scenario shares this process. It deliberately does NOT pass
// --useSharedGCHeap: the legacy runEndPhase reclamation site is the sole
// option-off behavior delta (I10 exemption) and is exactly what this checks:
//   retire -> legacy GC -> NOT freed by the retiring cycle -> legacy GC -> freed,
// plus the negative half: a conducted cycle's own periphery suspension never
// licenses bumpAndReclaim() (the reclaimer bracket does).
//
// Runnable in the no-JIT TSAN config and JIT-on unchanged (the scenario is
// C-level; this file only drives $vm.sharedHeapTest).
load("./resources/assert.js", "caller relative");

if (typeof $vm !== "undefined" && typeof $vm.sharedHeapTest === "function") {
    // All hard checks are RELEASE_ASSERTs inside the harness; `true` means
    // every one of them held.
    shouldBeTrue($vm.sharedHeapTest("epochReclaim", 1, 64), "epochReclaim");
    // Idempotent: a second run retires and drains a fresh batch.
    shouldBeTrue($vm.sharedHeapTest("epochReclaim", 1, 8), "epochReclaim (again)");
} else {
    // INTEGRATE-heap.md manifest item 8 ($vm.sharedHeapTest) is overlay-only;
    // on a tree without it this test is vacuous by design.
}
print("PASS");
