//@ requireOptions("--useJSThreads=1")
// lock-fairness: N threads contend ONE Lock in a tight loop for a fixed wall
// window; the test documents the observed fairness envelope rather than
// asserting strict fairness.
//
// The spec ALLOWS barging (an unparked acquirer may overtake parked waiters),
// so per-thread acquisition counts are expected to be unequal — possibly very
// unequal. What the spec does NOT allow is indefinite starvation: every
// contender must make progress. Detection is structured so that NO failure
// mode depends on a starved thread returning from lock.hold():
//
//   WATCHDOG (starvation, total OR mid-run):
//     * each worker publishes its acquisition count via property-path
//       Atomics.store after every acquisition and flips a per-thread done
//       flag when its loop exits; the MAIN thread — after opening the start
//       gate and BEFORE parking in joinAll — polls for ADVANCEMENT, not mere
//       nonzero-ness: if any not-yet-done thread's progress slot stops
//       increasing for more than STALL_MS (while the run is still open), or
//       any thread is still not done at HARD_CAP + grace, main throws a loud
//       error carrying the per-thread progress snapshot and the stalled
//       indices. This catches BOTH a waiter that never acquires (slot frozen
//       at 0) and the realistic wakeup-loss bug — unlock stops waking parked
//       waiters only after the run is underway (slot frozen at k >= 1).
//       Either way: a diagnostic assert, NOT an opaque harness-timeout hang.
//       CORROBORATION: the stalled-slot verdict additionally requires that
//       some OTHER thread advanced inside the victim's stall window (proof
//       the host was scheduling and the lock was cycling past the victim);
//       when ALL live threads freeze at once — indistinguishable from this
//       shared host descheduling the whole process for >STALL_MS — the
//       clocks are reset ONCE with a printed NOTE, and only a REPEATED
//       global freeze (or the hardCap+grace deadline) fails. A real global
//       wakeup-loss reproduces immediately; a host stall generally does not.
//   HARD assertions (after join):
//     * mutual exclusion holds: the owner canary is checked at section ENTRY
//       (previous holder still inside) and at section EXIT (a second holder
//       overwrote owner mid-section), with SECTION_WORK read-modify-writes
//       in between so an overlapping writer is observable across the whole
//       critical section, not a 3-statement window;
//     * lock-protected counters (total and the order-independent mix
//       checksum, ~SECTION_WORK RMWs per acquisition) match the values
//       recomputed from per-thread counts — lost updates from any overlap
//       integrate over ~1e5 acquisitions;
//     * the lock is free after all threads join.
//   DOCUMENTED-ENVELOPE assertion (in-window counts) with RETRY, never a
//   silent skip when the lock is demonstrably live:
//     * max/min in-window acquisition ratio <= FAIRNESS_ENVELOPE (10000x).
//       This bound is deliberately enormous — it is NOT a fairness
//       guarantee, it is a partial-starvation tripwire: a barging-but-live
//       lock lands orders of magnitude inside it, while a once-per-timeout
//       trickle blows through it.
//     * If some thread had ZERO in-window acquisitions (ratio undefined),
//       the test does NOT simply skip — that branch used to be weakest
//       exactly where monopolization is strongest. Instead it reruns one
//       fresh contention window. If on the retry every thread acquires
//       in-window, the envelope is asserted on the retry. If the SAME
//       symptom repeats — some thread at zero in-window while the busiest
//       thread piled up >= LIVENESS_FLOOR in-window acquisitions in BOTH
//       independent runs (proof the host was scheduling and the lock was
//       cycling) — the test FAILS as partial starvation. The only remaining
//       skip is the genuinely degenerate host: both runs ALSO show the
//       busiest thread below LIVENESS_FLOOR, i.e. nobody could run, which
//       no lock implementation can be blamed for.
//
// Load immunity: each worker's loop extends past the window up to a hard cap
// while its own count is still 0, so a worker descheduled by a saturated
// host for longer than the 2s window does not record an innocent zero. The
// envelope is computed from in-window counts only, so the extension does not
// distort the documented ratio.
//
// Stub-scheduling note (mirrors sync/ corpus): workers are self-sufficient —
// main opens the start gate BEFORE its watchdog poll / join, and workers'
// only blocking ops are the bounded gate wait + the Lock itself.
//
// Runtime: ~WINDOW_MS plus spawn/join overhead on a healthy run (a few
// seconds). Worst case = run 1 to its cap (~18s) plus one retry to its
// shorter cap (~11s) ~= 29s plus overhead, well inside the 120s timeout;
// the retry path only executes when run 1 saw a zero in-window count.
load("../resources/assert.js", "caller relative");

const THREADS = (typeof globalThis.SCALING_THREADS === "number"
                 && globalThis.SCALING_THREADS === (globalThis.SCALING_THREADS | 0)
                 && globalThis.SCALING_THREADS >= 2)
    ? globalThis.SCALING_THREADS | 0 : 4;
const WINDOW_MS = 2000;
const HARD_CAP_MS = 14000;        // run-1 liveness extension for a 0-count worker
const WATCHDOG_GRACE_MS = 4000;   // main waits this much past the hard cap
const RETRY_HARD_CAP_MS = 8000;   // retry run is shorter to bound total runtime
const RETRY_GRACE_MS = 3000;
const STALL_MS = 6000;            // not-done thread frozen this long => starved
const FAIRNESS_ENVELOPE = 10000;
const LIVENESS_FLOOR = 100;       // busiest thread in-window count proving the lock was live
const GATE_TIMEOUT_MS = 30000;
const SECTION_WORK = 8;           // RMWs per critical section (mix checksum)

const sleeper = { z: 0 };         // main's poll-sleep futex; never notified

// One full contention measurement: spawn THREADS workers on a fresh Lock,
// open the gate, watchdog for stalled progress, join, run all hard
// correctness assertions, and return the per-thread counts.
function runContention(tag, windowMs, hardCapMs, graceMs) {
    const lock = new Lock();
    const gate = { go: 0 };
    const shared = { owner: -1, overlaps: 0, total: 0, mix: 0 };

    // Per-thread progress + done slots, published with property-path Atomics
    // so the MAIN thread can observe a worker that stops advancing while
    // parked forever inside hold().
    const progress = {};
    const done = {};
    for (let i = 0; i < THREADS; ++i) {
        progress["t" + i] = 0;
        done["t" + i] = 0;
    }

    const workers = spawnN(THREADS, index => {
        // Park on the start gate so all contenders enter the window together
        // (without this, early spawns could burn their whole window before
        // late spawns exist, making "starvation" unmeasurable). Property-path
        // Atomics.wait parks with the GIL dropped; bounded so a lost notify
        // fails loudly instead of hanging.
        const r = Atomics.wait(gate, "go", 0, GATE_TIMEOUT_MS);
        if (Atomics.load(gate, "go") !== 1)
            throw new Error(tag + " worker " + index + ": start gate never opened (wait => " + r + ")");

        const start = Date.now();
        const deadline = start + windowMs;
        const hardCap = start + hardCapMs;
        let total = 0;
        let inWindow = 0;
        // Loop past the window while this thread has acquired NOTHING, up to
        // hardCap: a worker the OS descheduled for >windowMs records its
        // first acquisition late instead of an innocent zero. The envelope
        // uses in-window counts only, so this extension never inflates the
        // ratio.
        while (Date.now() < deadline || (total === 0 && Date.now() < hardCap)) {
            lock.hold(() => {
                // Exclusion canary, entry side: previous holder still inside.
                if (shared.owner !== -1)
                    shared.overlaps++;
                shared.owner = index;
                // Real shared work widens the observation window to the
                // whole section; mix is order-independent (commutative adds,
                // no 32-bit wrap at these magnitudes) so it is exactly
                // recomputable from per-thread counts — any lost update from
                // an overlapping holder breaks it.
                for (let k = 0; k < SECTION_WORK; ++k)
                    shared.mix = (shared.mix + index + k + 1) | 0;
                shared.total++;
                // Exclusion canary, exit side: a second holder that entered
                // anywhere during the section overwrote owner.
                if (shared.owner !== index)
                    shared.overlaps++;
                shared.owner = -1;
            });
            total++;
            if (Date.now() <= deadline)
                inWindow++;
            Atomics.store(progress, "t" + index, total);
        }
        Atomics.store(done, "t" + index, 1);
        return { total: total, inWindow: inWindow };
    });

    // Open the gate, THEN watchdog-poll progress BEFORE parking in join, so
    // a starved worker (parked forever inside lock.hold — whether it never
    // acquired at all OR stopped being woken mid-run) is reported as a loud
    // assert with a snapshot instead of an opaque 120s harness timeout.
    Atomics.store(gate, "go", 1);
    Atomics.notify(gate, "go");

    {
        const opened = Date.now();
        const watchdogDeadline = opened + hardCapMs + graceMs;
        const lastValue = new Array(THREADS).fill(-1);
        const lastChange = new Array(THREADS).fill(opened);
        const doneSeen = new Array(THREADS).fill(false);
        // CORROBORATION RULE: a frozen progress slot alone cannot distinguish
        // "parked forever inside lock.hold()" from "the OS descheduled this
        // worker for STALL_MS" — this machine is shared and >6s deschedules
        // under load are plausible. So a STARVATION verdict on a stalled slot
        // additionally requires that some OTHER thread advanced (progress
        // changed, or its done flag flipped) within the last STALL_MS: proof
        // the host was scheduling and the lock was cycling PAST the victim —
        // the same evidentiary standard the envelope check applies via
        // LIVENESS_FLOOR. When EVERY live thread is frozen simultaneously
        // (no corroboration), the first occurrence is treated as a suspected
        // whole-process host stall: clocks are reset once with a printed
        // NOTE. A real global wakeup-loss reproduces — the SECOND
        // uncorroborated global freeze fails, as does the hardCap+grace
        // deadline backstop.
        let hostStallGraceUsed = false;
        for (;;) {
            const now = Date.now();
            let allDone = true;
            const stalled = [];
            let anyAdvancedRecently = false;
            for (let i = 0; i < THREADS; ++i) {
                if (Atomics.load(done, "t" + i) === 1) {
                    if (!doneSeen[i]) {
                        doneSeen[i] = true;
                        lastChange[i] = now;
                    }
                    // A freshly finished worker is advancement evidence too.
                    if (now - lastChange[i] <= STALL_MS)
                        anyAdvancedRecently = true;
                    continue;
                }
                allDone = false;
                const v = Atomics.load(progress, "t" + i);
                if (v !== lastValue[i]) {
                    lastValue[i] = v;
                    lastChange[i] = now;
                }
                if (now - lastChange[i] <= STALL_MS)
                    anyAdvancedRecently = true;
                else
                    stalled.push(i);
            }
            if (allDone)
                break;
            if (stalled.length > 0 && !anyAdvancedRecently && !hostStallGraceUsed
                && now < watchdogDeadline) {
                hostStallGraceUsed = true;
                print("lock-fairness[" + tag + "]: NOTE — ALL live threads' progress"
                    + " slots frozen for >" + STALL_MS + "ms with no thread advancing"
                    + " (no corroboration that the lock was cycling); suspected"
                    + " whole-process host stall on this shared machine — resetting"
                    + " stall clocks ONCE (a real global wakeup-loss reproduces and"
                    + " will fail the second occurrence or the hardCap+grace backstop)");
                for (let i = 0; i < THREADS; ++i)
                    lastChange[i] = now;
                Atomics.wait(sleeper, "z", 0, 50);
                continue;
            }
            if (stalled.length > 0 || now >= watchdogDeadline) {
                const snapshot = [];
                const notDone = [];
                for (let i = 0; i < THREADS; ++i) {
                    snapshot.push(Atomics.load(progress, "t" + i));
                    if (Atomics.load(done, "t" + i) !== 1)
                        notDone.push(i);
                }
                throw new Error("lock-fairness[" + tag + "]: STARVATION — "
                    + (stalled.length > 0
                        ? ("thread(s) [" + stalled.join(",") + "] made no progress for >" + STALL_MS + "ms while still inside their loop"
                           + (anyAdvancedRecently
                               ? " WHILE other thread(s) kept advancing (corroborated: the host was scheduling and the lock was cycling past the victim)"
                               : " and ALL live threads were frozen AGAIN after one host-stall grace reset (global freeze reproduced — consistent with unlock waking no parked waiter at all)"))
                        : ("thread(s) [" + notDone.join(",") + "] still not done at hardCap+grace (" + (hardCapMs + graceMs) + "ms)"))
                    + "; per-thread acquisition snapshot=[" + snapshot.join(",") + "]"
                    + " (a frozen nonzero slot means unlock stopped waking an"
                    + " established parked waiter; a frozen zero slot means the"
                    + " waiter never acquired at all — either way it is likely"
                    + " parked forever inside lock.hold)");
            }
            // Sleep ~50ms with the GIL dropped (sleeper.z is never notified).
            Atomics.wait(sleeper, "z", 0, 50);
        }
    }

    // Every done flag is set, so every worker has returned from its loop and
    // joinAll is bounded by return/teardown only.
    const results = joinAll(workers);

    const totals = [];
    const windows = [];
    let sumTotal = 0;
    let expectedMix = 0;
    for (let i = 0; i < THREADS; ++i) {
        const r = results[i];
        shouldBeTrue(r !== null && typeof r === "object"
            && typeof r.total === "number" && r.total >= 0
            && typeof r.inWindow === "number" && r.inWindow >= 0,
            tag + ": thread " + i + " returned {total, inWindow}");
        totals.push(r.total);
        windows.push(r.inWindow);
        sumTotal += r.total;
        // Per acquisition, thread i adds sum_{k=0..SECTION_WORK-1}(i + k + 1)
        // = SECTION_WORK*i + SECTION_WORK*(SECTION_WORK+1)/2 to mix.
        expectedMix += r.total * (SECTION_WORK * i + SECTION_WORK * (SECTION_WORK + 1) / 2);
    }

    let minWindow = Infinity;
    let maxWindow = -Infinity;
    for (let i = 0; i < THREADS; ++i) {
        if (windows[i] < minWindow)
            minWindow = windows[i];
        if (windows[i] > maxWindow)
            maxWindow = windows[i];
    }

    print("lock-fairness[" + tag + "]: threads=" + THREADS + " window=" + windowMs + "ms"
        + " totals=[" + totals.join(",") + "]"
        + " inWindow=[" + windows.join(",") + "]"
        + " minW=" + minWindow + " maxW=" + maxWindow
        + " ratioW=" + (minWindow > 0 ? (maxWindow / minWindow).toFixed(2) : "inf"));

    // Mutual exclusion and lock-protected accounting.
    shouldBe(shared.overlaps, 0, tag + ": no critical-section overlap observed (entry or exit canary)");
    shouldBe(shared.total, sumTotal, tag + ": lock-protected total matches summed per-thread counts");
    shouldBe(shared.mix, expectedMix | 0,
        tag + ": lock-protected mix checksum matches recomputation from per-thread counts (lost update => overlap)");
    shouldBeFalse(lock.locked, tag + ": lock free after all contenders joined");

    // Liveness consistency: the watchdog already proved every thread kept
    // advancing until done; the joined totals must agree.
    shouldBeTrue(totals.every(c => c >= 1),
        tag + ": every thread must report >= 1 acquisition (watchdog saw progress; totals=["
        + totals.join(",") + "])");

    return { totals: totals, windows: windows, minWindow: minWindow, maxWindow: maxWindow };
}

const run1 = runContention("run1", WINDOW_MS, HARD_CAP_MS, WATCHDOG_GRACE_MS);
let envelopeRun = run1;
let envelopeTag = "run1";

// DOCUMENTED ENVELOPE: barging is allowed, strict fairness is not promised;
// this loose ratio bound only catches near-total monopolization (a
// once-per-park trickle). Computed over IN-WINDOW counts so the hard-cap
// liveness extension cannot distort it. A zero in-window count does NOT
// silently disarm the check: it triggers exactly one fresh retry window, and
// a REPEATED zero alongside a demonstrably live lock fails as partial
// starvation (see header).
if (run1.minWindow < 1) {
    print("lock-fairness: NOTE — run1 had a thread with 0 in-window acquisitions"
        + " (first acquisition landed after the " + WINDOW_MS + "ms window);"
        + " rerunning ONE fresh contention window to separate host stall from"
        + " lock-induced monopolization");
    const run2 = runContention("retry", WINDOW_MS, RETRY_HARD_CAP_MS, RETRY_GRACE_MS);
    envelopeRun = run2;
    envelopeTag = "retry";
    if (run2.minWindow < 1) {
        const lockWasLive = run1.maxWindow >= LIVENESS_FLOOR && run2.maxWindow >= LIVENESS_FLOOR;
        if (lockWasLive) {
            throw new Error("lock-fairness: PARTIAL STARVATION — in TWO independent "
                + WINDOW_MS + "ms windows some thread acquired ZERO times in-window"
                + " while the busiest thread acquired " + run1.maxWindow + " (run1) and "
                + run2.maxWindow + " (retry) times in-window; the host was scheduling and"
                + " the lock was cycling, so the victim's delay is the lock's doing"
                + " (monopolization beyond the documented envelope)."
                + " run1 inWindow=[" + run1.windows.join(",") + "]"
                + " retry inWindow=[" + run2.windows.join(",") + "]");
        }
        print("lock-fairness: NOTE — both runs ALSO had busiest-thread in-window counts"
            + " below " + LIVENESS_FLOOR + " (run1 maxW=" + run1.maxWindow
            + ", retry maxW=" + run2.maxWindow + "): the host could barely schedule"
            + " ANY contender, which no lock implementation can be blamed for —"
            + " envelope ratio skipped (liveness was still proven by the watchdog"
            + " in both runs)");
    }
}

if (envelopeRun.minWindow >= 1) {
    shouldBeTrue(envelopeRun.maxWindow <= envelopeRun.minWindow * FAIRNESS_ENVELOPE,
        "fairness envelope exceeded (" + envelopeTag + "): maxW=" + envelopeRun.maxWindow
        + " > " + FAIRNESS_ENVELOPE + " * minW=" + envelopeRun.minWindow
        + " (barging is legal, monopolization is not)");
}

// WOULD-FAIL-IF: the Lock implementation can starve a contender or break
// exclusion. The detection channels, honestly stated:
//   * STARVATION, total or mid-run (unlock path stops waking parked waiters
//     — whether from the first acquisition or only after a park queue is
//     established under load; park queue drops an entry; releasing thread
//     perpetually re-acquires without publishing the lock free): the starved
//     worker stops returning from lock.hold(), so its Atomics-published
//     progress slot stops ADVANCING while its done flag stays clear, and the
//     MAIN-THREAD watchdog throws a loud error with the per-thread snapshot
//     after STALL_MS (or at hardCap+grace) — a diagnostic assert, NOT a
//     silent joinAll hang killed by the harness timeout. The per-victim
//     verdict requires corroboration (another thread advancing during the
//     victim's stall window), so a >STALL_MS whole-process deschedule on
//     this shared host gets ONE clock-reset grace instead of a spurious
//     failure — while a global wakeup-loss, which reproduces, still fails on
//     the second freeze or the deadline backstop. The post-join totals >= 1
//     check is a consistency backstop, not the primary detector.
//   * PARTIAL starvation (a thread limps on a once-per-timeout trickle while
//     another monopolizes): blows the 10000x in-window envelope when every
//     thread has an in-window count; when the victim's in-window count is
//     ZERO (the severe case), one fresh retry window runs, and a repeated
//     zero while the busiest thread logged >= 100 in-window acquisitions in
//     BOTH runs FAILS as partial starvation — the skip branch survives only
//     for a host that could not schedule any contender at all.
//   * MUTUAL-EXCLUSION regression (two holders at once): caught by the
//     section-exit owner check (any overlapping holder that sets owner
//     during our SECTION_WORK-RMW section is observed at exit, regardless
//     of interleaving) and, independently and integrated over ~1e5
//     acquisitions, by lost updates breaking shared.total === sum and the
//     order-independent mix checksum.
