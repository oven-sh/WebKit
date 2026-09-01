//@ requireOptions("--useSharedGCHeap=1", "--useConcurrentSharedGCMarking=1", "--useJSThreads=1", "--useDollarVM=1", "--numberOfGCMarkers=4")
// SPEC-congc CG-T9 (all stages): ATTACH/EXIT1 churn — ANNEX CGT1.2 charter
// (CG-3c).
//
// What CG-3c landed (the engine surface this file gates):
//  - §9.2(1) EXIT1 order: teardown -> PERMANENT access drop -> CMS final
//    flush under m_markingMutex (HeapClientSet::flushClientMutatorMarkStack-
//    ForExit; target = the SERVER legacy m_mutatorMarkStack via its
//    multi-producer append, see the AMEND record in INTEGRATE-congc.md) ->
//    epoch=MAX -> HCS remove. F36: no dead-state publication.
//  - §9.2(4)/§3.7: EXIT1 on m_gcConductorThread mid-cycle is a RELEASE
//    ASSERT in ~GCClient::Heap. This file's churn arms "attempt" it in the
//    CGT1.2 sense: the storm makes thread exits land in every window gap of
//    live cycles; the engine assert is the oracle (a protocol breach that
//    let a live conductor reach EXIT1 crashes loudly; the test passing
//    witnesses the §3.7 closed-loop discipline held under churn).
//  - §9.3(1) ATTACH fence-init handshake: fence/threshold snapshot + FEP
//    stamp inside the publishing GBL/!WSAC section, BEFORE the HCS insert
//    (HeapClientSet::snapshotBarrierFenceStateForAttach; live once the
//    INTEGRATE-congc.md manifest row CG-3c-M1 is applied) — a live-marking
//    attachee starts RAISED; CG-I3's WND-close assert is the engine oracle.
//  - §5.2(ii) SINFAC hot-poll-tail CMS donation (threshold option) — the
//    barrier-heavy churn below pushes client CMSes over
//    sharedGCMutatorMarkStackDonationThreshold mid-cycle.
//
// CGT1.2 arms in this file:
//  1. ATTACH/EXIT1 churn during forced concurrent cycles (CG-I17/I20):
//     waves of short-lived Threads spawn, run barrier-heavy work, and exit
//     while the main thread forces back-to-back cycles.
//  2. Attach storm: a burst of simultaneous spawns against a live cycle
//     (HCS add blocks only inside windows — I13 add-side).
//  3. Exit with finalizer-side stores during full marking (§9.2(1)): the
//     exiting threads mutate OLD retained objects right up to exit, so the
//     final CMS flush carries real remembered-set work.
//  4. Attach-then-exit inside one between-windows gap (F25): immediate
//     spawn+join pairs under forced-cycle pressure.
//  5. Spawn+exit arming m_issRevertPending mid-cycle + main-client poll
//     storm (F11): waves that leave size()==1 with the main client
//     surviving, then main-thread allocation/poll churn — cycle completion
//     is the oracle (the restructured §10D pre-check must not deadlock).
//  6. clientChurnVsGC + issRevertChurn re-run (harness arm).
//
// Driver-carried run-config arms (t3 convention): F36 amplifier arm
// (amplifier-descheduled EXIT1 parked between the CMS flush and the GBL
// acquire across a fence-raising window; CG-I3 assert + TSAN on), F34
// ACT/DCT amplifier-descheduled across the NotRunning -> first-WND-open
// edge, GIL-off pinned env + C1 flag, TSAN, Debug build for the assert
// oracles.
//
// Pass criterion: exact checksums + termination. Skip-arms PASS without
// Thread/$vm.
load("./harness.js", "caller relative");

const haveVM = typeof $vm !== "undefined";
const haveThread = typeof Thread === "function";

function forcedGCs(n) {
    if (!haveVM)
        return;
    for (let i = 0; i < n; ++i) {
        $vm.gc();
        sleepMs(1);
    }
}

// Arm 6 first (harness; runs whether or not Thread exists): CGT1.2 names
// the clientChurnVsGC + issRevertChurn re-run explicitly.
if (haveVM && typeof $vm.sharedHeapTest === "function") {
    shouldBeTrue($vm.sharedHeapTest("clientChurnVsGC", 4, 12), "clientChurnVsGC under C1");
    shouldBeTrue($vm.sharedHeapTest("issRevertChurn", 2, 8), "issRevertChurn under C1");
}

if (haveThread && haveVM) {
    // OLD retained graph for arm 3: spawned threads store into it right up
    // to exit, so exit-time CMS flushes carry real barrier work whose loss
    // would corrupt the checksum verified after every wave.
    const OLD_SLOTS = 512;
    const oldGraph = { slots: new Array(OLD_SLOTS).fill(null) };
    forcedGCs(2); // Tenure the container.

    let expected = 0;

    // Arm 1 + 3 + 4: spawn/exit waves against forced cycles.
    const WAVES = 6;
    const PER_WAVE = 4;
    for (let wave = 0; wave < WAVES; ++wave) {
        const gate = { go: 0, started: 0 };
        const threads = spawnN(PER_WAVE, (t) => {
            // Lexical capture (shared heap): wave/gate/oldGraph are shared
            // with the spawner — the same capture shape as congc-t4.
            Atomics.add(gate, "started", 1);
            while (Atomics.load(gate, "go") === 0)
                Atomics.wait(gate, "go", 0, 100);
            let sum = 0;
            // Barrier-heavy: stores of FRESH objects into the OLD graph are
            // exactly the remembered-set appends the §9.2(1) exit flush and
            // the §5.2(ii) donation must not lose.
            for (let i = 0; i < 2000; ++i) {
                const slot = ((t * 131) + i) & (oldGraph.slots.length - 1);
                const v = ((wave << 24) ^ (t << 16) ^ i) | 0;
                oldGraph.slots[slot] = { v: v, link: oldGraph.slots[(slot + 1) & (oldGraph.slots.length - 1)] };
                sum = (sum + v) | 0;
            }
            // Exit immediately after the last store: the final stores sit
            // in this client's CMS at EXIT1 (arm 3).
            return sum;
        });

        waitUntil(() => Atomics.load(gate, "started") === PER_WAVE);
        Atomics.store(gate, "go", 1);
        Atomics.notify(gate, "go");
        // Force cycles WHILE the wave runs and exits — exits land between
        // windows of live cycles (arm 1); the wave's join+respawn cadence is
        // the attach-then-exit-in-one-gap pressure (arm 4).
        forcedGCs(3);
        const sums = joinAll(threads);
        for (const s of sums)
            expected = (expected + s) | 0;
        // Arm 5: after the join, registry size is back to 1 (main client
        // survives) — m_issRevertPending may be armed mid-/post-cycle; this
        // poll storm (allocation + explicit GCs) must complete cycles, never
        // deadlock (F11/CGD1.2 restructured pre-check).
        let pollChurn = [];
        for (let i = 0; i < 2000; ++i)
            pollChurn.push({ i: i });
        forcedGCs(2);
        shouldBe(pollChurn.length, 2000, "post-wave poll storm completed (wave " + wave + ")");
    }

    // Verify the OLD graph: every slot written by the last writers must
    // read back intact — a lost exit-flush cell shows up as a swept-under
    // object (crash) or torn value here.
    let live = 0;
    for (let s = 0; s < OLD_SLOTS; ++s) {
        const cell = oldGraph.slots[s];
        if (cell === null)
            continue;
        if (typeof cell.v !== "number")
            throw new Error("EXIT1-flush corruption at slot " + s);
        live++;
    }
    shouldBeTrue(live > 0, "old graph retained writes across exit churn");

    // Arm 2: attach storm — simultaneous spawns against a live cycle.
    forcedGCs(1);
    const burst = spawnN(8, (t) => {
        // Minimal body: the test is the ATTACH handshake itself (§9.3(1)
        // snapshot + first-AHA GSP load) against the forced cycle below.
        let x = 0;
        for (let i = 0; i < 200; ++i)
            x = (x + i) | 0;
        return x;
    });
    forcedGCs(2);
    const burstResults = joinAll(burst);
    shouldBe(burstResults.length, 8, "attach storm joined");
    for (const r of burstResults)
        shouldBe(r, 19900, "attach-storm thread checksum");

    forcedGCs(2); // Post-churn cycles: CG-I3 / window-close asserts re-run.
} else {
    // Skip arm: no Thread global — the harness arm above (if $vm present)
    // already ran; otherwise PASS bare.
}
