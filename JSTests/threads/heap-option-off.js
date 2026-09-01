//@ requireOptions("--useDollarVM=1")
// SPEC-heap.md T10: I10 — with --useSharedGCHeap left at its default (off),
// fast/slow allocation paths execute today's code: TLC bypassed, server
// allocators populated, legacy collection protocol (incl. concurrent
// marking), MutatorSlowPathLocker a no-op. The sole option-off behavior
// delta is the legacy runEndPhase hook + epoch-reclaim call (§9 note),
// which heap-epoch-reclaim.js covers positively; here we check:
//
//   1. The shared-mode harness scenarios REFUSE to run (manifest-8 guard /
//      requireSharedHeapOption), so nothing shared-mode can leak into the
//      default configuration.
//   2. epochReclaim still passes (the I10-exempt legacy reclamation works
//      with the option off).
//   3. Plain allocation + GC churn is deterministic.
//
// PREMISE SELF-CHECK: this test is a DEFAULT-configuration witness. jsc
// applies JSC_<option> environment variables in Options::initialize(),
// before argv, so an ambient export like JSC_useSharedGCHeap=true (e.g. a
// rung that pins its whole configuration via env) silently inverts the
// premise: the refusal asserts would then report a FAIL that is not an
// engine regression. We deliberately do NOT pin --useSharedGCHeap=0 on the
// header: that argv pin would break the {useVMLite, useSharedAtomStringTable,
// useSharedGCHeap} GIL-off trio validation (Options.cpp forces useThreadGIL
// back on), turning the run into a hybrid configuration that certifies
// nothing — and it would convert this default-config witness into an
// explicit-value test in a project that moves option defaults. Instead we
// query the EFFECTIVE option at runtime and emit an explicit SKIP marker
// (THREADS-PREMISE-SKIP, recognized by Tools/threads/run-tests.sh and
// counted as SKIP, not PASS) so the contradiction surfaces loudly and the
// conflict is actionable at the rung definition, instead of fake-passing
// or fake-failing here.
//
// The QUANTITATIVE half of the I10 gate is Tools/threads/bench-gate.sh over
// JSTests/threads/bench/ (option off by default) plus
// JSTests/threads/heap-bench-allocation.js — see INTEGRATE-heap.md (T10).
load("./resources/assert.js", "caller relative");

if (typeof jscOptions === "function" && jscOptions().useSharedGCHeap) {
    // Ambient configuration (most likely a JSC_useSharedGCHeap env export)
    // has the option ON: the option-off premise is inverted, so neither the
    // refusal asserts nor the option-off churn determinism check certify
    // anything in this run. Surface it as an explicit skip — the fix belongs
    // at whatever pinned the env, not here.
    print("THREADS-PREMISE-SKIP: useSharedGCHeap is ON in the effective configuration"
        + " (ambient JSC_* env?); heap-option-off.js is a default-configuration"
        + " (option-off) witness and cannot run meaningfully under it.");
} else {
    if (typeof $vm !== "undefined" && typeof $vm.sharedHeapTest === "function") {
        // 1. Shared-mode scenarios refuse when the option is off (either the
        //    $vm guard throws or the harness returns false; both count).
        for (const scenario of ["allocationStorm", "clientChurnVsGC", "issRevertChurn"]) {
            let refused = false;
            try {
                refused = !$vm.sharedHeapTest(scenario, 2, 4);
            } catch {
                refused = true;
            }
            shouldBeTrue(refused, scenario + " must refuse with --useSharedGCHeap off");
        }

        // 2. The legacy reclamation site works option-off (I10 exemption).
        shouldBeTrue($vm.sharedHeapTest("epochReclaim", 1, 16), "epochReclaim option-off");
    }

    // 3. Deterministic allocation/GC churn on the legacy path.
    let sum = 0;
    for (let i = 0; i < 20000; ++i) {
        const o = { a: i, b: [i, i + 1] };
        sum += o.a + o.b[1];
    }
    if (typeof gc === "function")
        gc();
    shouldBe(sum, 400000000); // sum of (2i + 1) for i in [0, 20000)
    print("PASS");
}
