//@ requireOptions("--useSharedGCHeap=1", "--useConcurrentSharedGCMarking=1", "--useJSThreads=1", "--useDollarVM=1", "--numberOfGCMarkers=4")
// SPEC-congc CG-T11 (all stages): diagnostics-as-asserts — ANNEX CGT1.9
// charter (CG-3a).
//
// The §2.4 diagnostics this file drives through the C1 shape:
//  - freelisted-block check per WND-open: stopThePeriphery()'s post-flush
//    forEachBlock walk (no block may remain freelisted after the step-5
//    stopAllocating() flush). Under stage C1 stopThePeriphery runs at EVERY
//    window open (the finishChangingPhase suspend edge out of Concurrent =
//    WND-reopen), so each Concurrent round-trip re-executes the check — this
//    file's GC pressure forces many windows per cycle.
//  - endMarking root-liveness check: the conservative-stack-root snapshot
//    walk before m_objectSpace.endMarking() retires the newlyAllocated
//    version (see congc-t4's charter note).
//  Both are live for every ISS cycle in the current tree (RELEASE-grade
//  fix-shared-heap-corruption instrumentation, which is STRONGER than the
//  chartered debug-gated form); the re-gating to stage-flag-conditioned
//  debug asserts is recorded as open in INTEGRATE-congc.md.
//
// F37 A4-site walk (CGA1 A4; landed at CG-2 as the ASSERT_ENABLED block in
// runEndPhase): after m_helperClient.finish(), strictly BEFORE the first
// conductor-context writeBarrier batch — i.e. BEFORE
// iterateExecutingAndCompilingCodeBlocks barriers executing CodeBlocks —
// every client CMS must already be empty (the final window's WND-open drain
// emptied them; WSAC bars client appends since). This file arms the walk
// WITH EXECUTING CODEBLOCKS PRESENT: N threads sit in hot functions while
// full cycles run, so the runEndPhase iteration sees genuinely executing
// CodeBlocks and the A4 ordering (CMS-empty walk first, next-cycle-grey
// conductor appends second) is exercised rather than vacuous.
//
// Also exercised (CG-3a machinery): the CGP1 counter-balance debug assert
// after m_helperClient.finish() (active == waiting == paused == 0 — the F17
// counter-leave fixes are exactly what makes it hold at every cycle end).
load("./harness.js", "caller relative");

if (typeof Thread === "function" && typeof $vm !== "undefined") {
    const N = 3;
    const CYCLES = 8;
    const gate = { go: 0, started: 0, stop: 0 };

    // Hot function: enough body to earn a CodeBlock worth executing, called
    // in a tight loop so threads are INSIDE it (executing, not merely
    // compiled) whenever a conducted cycle's runEndPhase iterates executing
    // CodeBlocks (F37).
    function hot(seed, sink) {
        let x = seed | 0;
        for (let i = 0; i < 64; ++i) {
            x = (x * 1103515245 + 12345) | 0;
            if ((x & 7) === 0)
                sink.ref = { v: x }; // barriered store: keeps each thread's CMS populated under C1R
        }
        return x;
    }

    const sinks = [];
    for (let t = 0; t < N; ++t)
        sinks.push({ ref: null, tag: t });
    $vm.gc(); // age the sinks so the hot-loop stores are old->new barriers

    const threads = spawnN(N, (t) => {
        const sink = sinks[t];
        Atomics.add(gate, "started", 1);
        while (Atomics.load(gate, "go") === 0)
            Atomics.wait(gate, "go", 0, 100);
        let acc = t;
        let iterations = 0;
        while (Atomics.load(gate, "stop") === 0) {
            acc = hot(acc ^ iterations, sink);
            iterations++;
            if ((iterations & 1023) === 0) {
                // Allocation keeps this client an active mutator across
                // windows (didRun fold + CMS drain at each WND-open).
                const o = { it: iterations, t };
                if (o.t !== t)
                    throw new Error("allocation corruption");
            }
        }
        return iterations;
    });

    waitUntil(() => Atomics.load(gate, "started") === N);
    Atomics.store(gate, "go", 1);
    Atomics.notify(gate, "go", Infinity);

    // Drive CYCLES full collections while the threads execute hot code. Each
    // cycle: windows open/close (freelisted-block check per open), marking
    // terminates (endMarking root-liveness walk), end phase runs the A4
    // CMS-empty walk + CGP1 counter assert with executing CodeBlocks live.
    for (let c = 0; c < CYCLES; ++c) {
        let churn = 0;
        for (let i = 0; i < 8000; ++i)
            churn += ({ v: i }).v;
        shouldBe(churn, 31996000);
        $vm.gc();
    }

    Atomics.store(gate, "stop", 1);
    const iterationCounts = joinAll(threads);
    for (let t = 0; t < N; ++t)
        shouldBeTrue(iterationCounts[t] > 0, "thread " + t + " executed hot code");

    // The sinks' last leaves must have survived every cycle (their only
    // reference is the barriered hot-loop store).
    let populated = 0;
    for (let t = 0; t < N; ++t) {
        shouldBe(sinks[t].tag, t);
        if (sinks[t].ref !== null) {
            shouldBeTrue((sinks[t].ref.v | 0) === sinks[t].ref.v, "sink leaf intact");
            populated++;
        }
    }
    shouldBeTrue(populated > 0, "at least one sink leaf survived");

    // One more synchronous full GC after the threads exited: the cycle-end
    // asserts must also hold with zero running mutator threads.
    $vm.gc();
} else if (typeof $vm !== "undefined" && typeof $vm.sharedHeapTest === "function") {
    // Reduced config: still push cycles through the diagnostics.
    shouldBeTrue($vm.sharedHeapTest("syncRequesterStorm", 3, 12), "syncRequesterStorm under C1");
    $vm.gc();
}
print("PASS");
