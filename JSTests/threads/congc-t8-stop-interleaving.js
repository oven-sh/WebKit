//@ requireOptions("--useSharedGCHeap=1", "--useConcurrentSharedGCMarking=1", "--useJSThreads=1", "--useDollarVM=1", "--numberOfGCMarkers=4")
// SPEC-congc CG-T8: JSThreads-stop interleaving — ANNEX CGT1.1 charter
// (CG-3b), PLUS the [r34] SPEC-ungil-history F-A item (4) wedged-marker arm
// (see the PENDING note below) and the F-B/G-B attribution-only storm
// reading.
//
// What CG-3b landed (the engine surface this file gates):
//  - Heap::pauseConcurrentMarkingForForeignStop(): BOTH JSThreadsStopScope
//    ctors, after the GCL is HELD (watchdog ctor: after a SUCCESSFUL
//    tryLock only), pause the HelperDrain markers when
//    m_currentPhase != NotRunning. [r34] F-A item (1): the pause is a TIMED
//    wait sampling watchdogAssertStopProgress(requestStart, vm) per 1ms
//    quantum — a wedged marker batch fail-stops ON THE CONDUCTOR ITSELF
//    (with F-A item-(3) VM attribution), never an unwatched hang.
//  - ~JSThreadsStopScope resumes the markers strictly BEFORE releasing GCL
//    (dtor order NORMATIVE: no WND-open with paused markers).
//  - F45 fairness (CG-1 counter, verified live here): the WND-open blocking
//    re-acquire abstains while m_foreignGCLWaiters != 0 (CG-I26).
//  - F46 atom pin (per-window install/restore; CG-I27 debug-null between
//    windows) and the F35/CGD5.1 per-cycle rebias pin (final window of the
//    first Sealed Full cycle, post-reclaim, pre-ISB-bump).
//
// Pass criterion for every live arm: deterministic checksums stay EXACT and
// the file terminates — any pause/resume wedge surfaces as the 30s stop
// watchdog fail-stop (crash, loud), any lost marker work as a checksum
// mismatch or heap corruption crash, any CG-I27 violation as a deterministic
// null-atom-table crash in ASSERT builds.
//
// Run-config arms carried by the drivers, NOT by extra code here (the t3
// convention):
//  - F46 logGC arm: the SAME file with JSC_logGC=true — the conductor's
//    between-window dataLog/logGC paths must take no atom ops (CG-I27
//    debug-null crashes otherwise; ANNEX CGD7.3).
//  - F35 sub-arm + F43 GIL-off half: the SAME file under the GIL-off env
//    (JSC_useJSThreads=1 JSC_useThreadGIL=0 JSC_useVMLite=1
//    JSC_useSharedAtomStringTable=1 JSC_useSharedGCHeap=1
//    JSC_useThreadGILOffUnsafe=1) + the C1 flag: the thread-exit churn below
//    retires TIDs and seals rebias snapshots; the forced Full cycles must
//    flip Sealed->Restamped under WSAC in a final window, before any TID
//    reissue (CG-I23; engine-side asserts carry the witness).
//  - TSAN: Tools/threads/tsan/run-corpus-tsan.sh with
//    --useConcurrentSharedGCMarking=1 (suppressions = documented
//    RACY-TOLERATED rows only, CGA1/CGN1).
//  - Amplifier (Tools/threads/amplify.sh): perturb() sits at the rebias
//    pre-flip stall point and the window-edge slow paths; this file is the
//    amplifier's CG-T8 target workload.
//
// KNOWN-RED RECORD + FIX (amend pass, 2026-06-12): Arm 1 below was observed
// RED on a freshly built Release jsc under --useSharedGCHeap=1
// --useConcurrentSharedGCMarking=1 — all four harness scenarios crashed at
// the FIRST conducted collection with the runBeginPhase fail-stop
// "SlotVisitor should think that GC should terminate before constraint
// solving" (a numberOfGCMarkers=1 rerun dumped
// m_sharedMutatorMarkStack->isEmpty(): false; the same scenarios PASS with
// the C1 flag off). Root cause was NOT CG-3b's pause/resume (paused=0,
// ShouldPause=false at crash; syncRequesterStorm takes no stop scopes and
// still crashed): the CG-2 §5.2(i) WND-open CMS drain ran at the PRE-CYCLE
// FirstWindow open and pre-loaded m_sharedMutatorMarkStack before
// runBeginPhase's didReachTermination() precondition (SlotVisitor::hasWork
// counts the shared stacks; with >1 marker the freshly-armed helpers stole
// the pre-cycle cells, so the crash dump recomputed
// didReachTermination()=true). FIXED in this same amend
// (Heap::openSharedGCStopWindow): the drain target is now OPEN-KIND SPLIT —
// pre-cycle opens (FirstWindow / TicketDrainSuccessor) drain into the
// server legacy m_mutatorMarkStack (the landed pre-cycle barrier route,
// still ahead of the window's first constraint pass via
// MarkStackMergingConstraint, so §5.2(i)'s order holds); only the mid-cycle
// Reentry open feeds m_sharedMutatorMarkStack. NOT YET RE-RUN: the amend
// slice is write-only/no-builds — the builder loop must rebuild and re-run
// this file under (i) the //@ line's numberOfGCMarkers=4, (ii) a
// --numberOfGCMarkers=1 arm, AND (iii) the GIL-off env arm above
// (JSC_useJSThreads=1 JSC_useThreadGIL=0 JSC_useVMLite=1
// JSC_useSharedAtomStringTable=1 JSC_useSharedGCHeap=1
// JSC_useThreadGILOffUnsafe=1 + the C1 flag) before calling the gate
// green. Until those runs are recorded, this gate is
// KNOWN-RED-now-FIX-PENDING, not green.
//
// PENDING arms (recorded in docs/threads/INTEGRATE-congc.md):
//  - [r34] F-A item (4) WEDGED-MARKER AFFIRMATIVE ARM: proving the
//    fail-stop fires on the conductor itself requires (a) a marker-wedge
//    injection hook (a SharedHeapTestHarness scenario that parks one
//    HelperDrain helper outside its checkpoint while a stop scope lands
//    mid-cycle) and (b) an expected-crash run mode —
//    stopTheWorldWatchdogTimeout is constexpr 30s (JSThreadsSafepoint.cpp)
//    and a real marker cannot be wedged indefinitely from JS. The engine
//    leg is landed and structurally samples per 1ms quantum
//    (Heap::pauseConcurrentMarkingForForeignStop); this file's live arms
//    witness the NEGATIVE half: mid-cycle stop scopes with markers
//    churning complete promptly, no fail-stop at any legitimate depth
//    (the G-B attribution-only storm reading — no fan-in cap exists, so
//    "no fire below cap" is deliberately NOT claimed).
//  - F40 (BL1.8 NL-drop): the m_nativeLockDepth slot does not exist in
//    this tree yet (nativeaffinity owner) — same disposition as the
//    openSharedGCStopWindow CG-I19 comment.
load("./harness.js", "caller relative");

if (typeof Thread === "function" && typeof $vm !== "undefined" && typeof $vm.sharedHeapTest === "function") {
    // ---------------------------------------------------------------
    // Arm 1 — harness stop-scope interleavings, re-run under the C1 flag
    // (CGT1.1: "jsThreadsStopVsGCRequester re-run per stage"). These drive
    // the BLOCKING ctor (SharedHeapTestHarness C-level scenarios) and the
    // watchdog ctor against live conductors; with
    // useConcurrentSharedGCMarking on, any scope landing between windows
    // takes the §9.1(2) pause/resume bracket.
    shouldBeTrue($vm.sharedHeapTest("jsThreadsStopVsGCRequester", 4, 24), "jsThreadsStopVsGCRequester");
    shouldBeTrue($vm.sharedHeapTest("gcDuringDebuggerPark", 3, 16), "gcDuringDebuggerPark");
    shouldBeTrue($vm.sharedHeapTest("debuggerStopDuringSharedGC", 3, 16), "debuggerStopDuringSharedGC");
    shouldBeTrue($vm.sharedHeapTest("syncRequesterStorm", 4, 16), "syncRequesterStorm (CG-I21 storm)");

    // ---------------------------------------------------------------
    // Shared marking workload: an OLD graph big enough that forced cycles
    // schedule Concurrent windows (numberOfGCMarkers=4), so the stop arms
    // below can land between windows and against helpers mid-batch
    // (CG-I22). Aged once so the storm stores are barrier-relevant.
    const ROWS = 256;
    const old = [];
    for (let r = 0; r < ROWS; ++r)
        old.push({ a: r, b: 2 * r, ref: null, pad: "p" + (r & 15) });
    $vm.gc();

    const gate = { go: 0, stop: 0, started: 0, classAFires: 0 };

    // ---------------------------------------------------------------
    // Arm 2 — F18/F43/F45: sibling threads storm Class-A §A.3 stops
    // (haveABadTime on FRESH globals — each is a full watchpoint-fire stop
    // window via the jettison bracket, i.e. the WATCHDOG ctor path) while
    // the main thread forces back-to-back cycles. Interleavings produced:
    //  - stop-scope ctor between windows (tryLock succeeds BY DESIGN,
    //    §9.1(1)) -> §9.1(2) pause with helpers mid-batch;
    //  - stop-scope ctor vs the WND-open GCL re-acquire -> F45 abstention
    //    (CG-I26): the waiter must win within poll quanta — a starved
    //    waiter is a deterministic 30s fail-stop, i.e. a test crash;
    //  - F43/CG-I25: the §A.3 conductor's fire bodies take DeferGC, run
    //    write barriers and allocate mid-GC-cycle (AB-21 access
    //    re-acquire; AB-10 weak-sweep license) — composition facts
    //    verified engine-side, exercised here.
    const stormThreads = spawnN(3, (t) => {
        Atomics.add(gate, "started", 1);
        while (Atomics.load(gate, "go") === 0)
            Atomics.wait(gate, "go", 0, 100);
        let fires = 0;
        while (Atomics.load(gate, "stop") === 0) {
            // A fresh global per fire keeps the Class-A stop repeatable
            // (haveABadTime is one-shot per realm).
            const g = $vm.createGlobalObject();
            $vm.haveABadTime(g);
            ++fires;
            // Barrier-heavy stores into the OLD graph between fires: the
            // §5.2 CMS path rides every window the stops interleave with.
            for (let r = 0; r < ROWS; ++r)
                old[r].ref = { v: (t << 20) | (fires << 8) | (r & 255) };
        }
        Atomics.add(gate, "classAFires", fires);
        return fires;
    });

    waitUntil(() => Atomics.load(gate, "started") === 3, 30000);
    Atomics.store(gate, "go", 1);
    Atomics.notify(gate, "go");

    // Back-to-back forced cycles (F45 window storm against registered
    // waiters; F46: every window installs/restores the atom table, debug
    // builds null it between windows — CG-I27 violations crash here).
    for (let i = 0; i < 12; ++i) {
        $vm.gc();
        // Mutate between cycles so marking always has live work and the
        // next cycle schedules Concurrent windows again.
        for (let r = 0; r < ROWS; r += 7)
            old[r].b = old[r].a * 2;
    }

    Atomics.store(gate, "stop", 1);
    const fireCounts = joinAll(stormThreads);
    // GIL-OFF-ONLY EXPECTATION (gated 2026-06-12, A-t8assert; the KNOWN-RED
    // Arm-1 "GIL-on reading" in MEGA-RUN-RESULTS.md): "every storm thread
    // fired >= 1 Class-A stop MID-storm" is enforced by no machinery GIL-on —
    // the Class-A §A.3 thread-granular conductor windows this arm interleaves
    // are vm.gilOff()-gated (JSThreadsSafepoint.cpp; GIL-on takes the legacy
    // serialized path), and the cooperative GIL gives no fairness guarantee
    // that a storm thread is scheduled between the main thread's back-to-back
    // forced cycles before stop=1 lands (deterministically 0 fires today).
    // GIL-on the storm still runs (spawn/join/checksum oracles above and
    // below stay live); only the per-thread fire-count claim is GIL-off.
    // Mode probe is $vm.useThreadGIL() — the post-U0-validation EFFECTIVE
    // mode, the documented premise probe for the threads corpus (JSDollarVM).
    if (!$vm.useThreadGIL())
        shouldBeTrue(fireCounts.every((n) => n >= 1), "every storm thread completed >= 1 Class-A stop mid-storm");

    // ---------------------------------------------------------------
    // Arm 3 — thread-exit churn vs forced Full cycles (F35 feeder; CG-T9
    // adjacency): spawned threads exit while cycles run, retiring TIDs.
    // Under the GIL-off driver re-run this is what seals rebias snapshots
    // and arms the CGD5.1 final-window flip; GIL-on it is interleaving
    // churn (exits landing between windows, §9.2 ordering).
    for (let round = 0; round < 4; ++round) {
        const churn = spawnN(2, (t) => {
            let s = 0;
            const local = [];
            for (let i = 0; i < 4000; ++i) {
                local.push({ i, t });
                s += i;
            }
            return s;
        });
        $vm.gc(); // Full cycle with exits in flight / just landed.
        const sums = joinAll(churn);
        shouldBe(sums[0], 7998000);
        shouldBe(sums[1], 7998000);
    }

    // ---------------------------------------------------------------
    // Post-storm oracle: the OLD graph survived every interleaving intact
    // (a lost CMS cell / mis-paused marker under-marks: a later sweep
    // frees a reachable row and this walk reads garbage or crashes).
    let checksum = 0;
    for (let r = 0; r < ROWS; ++r) {
        shouldBe(old[r].a, r);
        shouldBe(old[r].b, 2 * r);
        checksum += old[r].a;
    }
    shouldBe(checksum, (ROWS - 1) * ROWS / 2);

    // One final clean conduct through the full window machinery.
    $vm.gc();
    shouldBeTrue(true, "post-storm full GC completed");
}
print("PASS");
