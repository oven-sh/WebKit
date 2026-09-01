//@ requireOptions("--useJSThreads=1", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1", "--useDollarVM=1")
// MC-AINT S4 (docs/threads/cve/map-MC-AINT.md): parked-at-poll resume across
// a Class-A fire — SPEC-jit I21(b). At authoring time I21(b) was specified
// but UNIMPLEMENTED (DFGByteCodeParser.cpp emitted CheckTraps WITHOUT an
// invalidation point when usePollingTraps is forced); it has since LANDED
// (handleCheckTraps now emits ExitOK + InvalidationPoint after every
// flag-on CheckTraps — see the AB-10 closure banner in-function and the
// CLOSED 2026-06-10 entry in map-MC-AINT.md S4), so this test is the
// standing I21(b) regression test.
//
// Mechanism (the async-interruption-at-an-unsafe-point shape, with the
// cooperative stop itself as the interruption): GIL-off, a Class-A
// watchpoint fire runs as an STWR. Reader threads hot in DFG/FTL code park
// at their CheckTraps polls; the fire falsifies the watched fact
// (transitionThreadLocal/writeThreadLocal) and jettisons their elided code;
// on resume each reader continues at the instruction AFTER the poll and may
// execute E1/E2-elided butterfly accesses against the now-false fact —
// e.g. an E1-elided flat read on a butterfly that became shared/segmented
// during the stop (the always-emitted mask does NOT detect a regime
// change). I21(b) exists precisely to forbid this window.
//
// Shape: each round builds a fresh object family owned by a dedicated owner
// thread; reader threads tier up on elided property reads carrying disjoint
// sentinel sets; then a FOREIGN thread performs the first foreign
// write/transition on the hot objects (synchronous fires + jettison under
// STWR — NOT the deferred-fire path, which mc-code-deferred-fire-stale-
// window.js covers; here publication ordering is correct by construction,
// so any oracle violation implicates the RESUME side) and keeps mutating
// (adds that grow/segment the out-of-line storage) while readers run on.
//
// Oracle: o.alpha only ever yields ALPHA-set values, o.beta only BETA-set
// values (or post-takeover sentinels, also disjoint). A cross-sentinel,
// undefined, hole, or torn value = stale elided code executed past a poll
// after its watchpoint fired.
//
// The window is poll-park-to-next-invalidation-boundary — scheduler-
// dependent — so this is an AMPLIFIER-READY race test, not deterministic:
// bounded rounds here, the amplifier widens the window (arm64 weak ordering
// helps the attacker). EXECUTED POST-UNGIL ONLY: under the phase-1 GIL the
// sole mutator runs fires inline and is never parked at a poll across one,
// closing the window by construction.
load("../harness.js", "caller relative");

// ---- GIL-mode gate (SPEC-api Deviation 9) ----
// Under the phase-1 GIL, preemption is COOPERATIVE-ONLY: the 5.2 blocking
// primitives are the only yield points (SPEC-api Deviation 9; G23/G24 —
// harness.js:47-50 records the same rule for waitUntil). Every loop in this
// test spins or runs hot WITHOUT a blocking primitive by design (the hot
// elided read loop IS the probed surface; inserting parks would gut the
// poll-resume window the oracle exists to catch). GIL-on that means the
// main driver legitimately starves the readers/foreign threads — zero
// progress is the DOCUMENTED scheduling model, not a defect — so the
// progress assertions below (checks > 0, foreignRounds > 0) assert
// something Deviation 9 does not promise. And per the header, the probed
// window itself does not exist GIL-on (the sole mutator runs fires inline
// and is never parked at a poll across one). So: read the EFFECTIVE GIL
// mode from $vm.useThreadGIL() (the post-U0-validation option value — the
// serialization mode the VM actually runs under) and premise-skip GIL-on.
// MODE-DERIVED, not behavioral: the previous probe here (spawn a thread,
// watch for progress against a spinning main thread within a 2s deadline)
// could misfire on a saturated host — a GIL-off run whose probe thread
// missed the window would silently premise-skip the exact test that pins
// the AB-10/I21(b) closure, converting a GIL-off lane into a vacuous
// skip with no failure surfaced. $vm is guaranteed by the requireOptions
// header (--useDollarVM=1).
{
    if ($vm.useThreadGIL()) {
        print("THREADS-PREMISE-SKIP: cooperative phase-1 GIL enabled"
            + " ($vm.useThreadGIL() === true, post-U0-validation effective"
            + " mode; SPEC-api Deviation 9); the I21(b) poll-resume window"
            + " this test probes is closed by construction GIL-on and the"
            + " test's spin loops cannot make cross-thread progress under"
            + " cooperative-only scheduling.");
        quit();
    }
}

const READERS = 3;
// ROUNDS halved 200 -> 100 (2026-06-10, cve-aint-timeout-budget): the
// I21(b) mechanism is CLOSED (map-MC-AINT.md S4, 20/20 + 40/40 runs); at
// 200 rounds the Debug GIL-off build PASSes semantically but takes ~143s
// wall on a quiet 64-core host (measured 2026-06-10), past the pinned 120s
// harness budget (Tools/threads/run-tests.sh TEST_TIMEOUT_SECS), so the
// gate red was rc=124, not an oracle hit. Per rule 1 the WINDOW is
// untouched: READS_PER_ROUND, the owner 20000-rewrite churn loop, and the
// 24-add foreign growth are unchanged — only the number of independent
// per-round trials is reduced. At ROUNDS=100 the measured wall time is
// ~72s under the same conditions. Do NOT raise the pinned timeout instead
// (it is the hang-class detector for the rest of the suite); if a loaded
// host still margins out, drop to ROUNDS = 80 with the window unchanged.
// Note: no option-level kill-check exists for this sentinel —
// --forceUnlinkedDFG=1 re-induces bare CheckTraps but suppresses the TTL
// elision itself, making the oracle vacuous; re-validating detection power
// requires a deliberate I21(b) revert.
const ROUNDS = 100;
const READS_PER_ROUND = 50000;

const ALPHA_BASE = 1000000; // owner-phase o.alpha values
const BETA_BASE = 2000000;  // owner-phase o.beta values
const FOREIGN_ALPHA = 3000000; // post-foreign-takeover o.alpha values
const FOREIGN_BETA = 4000000;  // post-foreign-takeover o.beta values
const SPAN = ROUNDS + 8;

function inSet(v, base) { return typeof v === "number" && v >= base && v < base + SPAN; }

const box = { o: null, round: -1 };
const gate = { ready: 0, go: 0, done: 0, stop: 0 };

function freshTarget(round) {
    // Out-of-line properties (inline capacity exhausted by filler) so reads
    // go through the tagged butterfly — the surface E1/E3 elision guards.
    const o = {};
    for (let i = 0; i < 8; ++i)
        o["filler" + i] = i;
    o.alpha = ALPHA_BASE + round;
    o.beta = BETA_BASE + round;
    return o;
}

// Hot read kernel; tier-up happens against owner-thread-local structures
// whose TTL sets are valid+watched => E1/E2/E3 elision in DFG/FTL.
function readPair(o) {
    return [o.alpha, o.beta];
}

const readers = [];
for (let r = 0; r < READERS; ++r) {
    readers.push(new Thread(() => {
        let checks = 0;
        let lastRound = -1;
        while (Atomics.load(gate, "stop") === 0) {
            const o = box.o;
            if (o === null) continue;
            for (let i = 0; i < READS_PER_ROUND; ++i) {
                const [a, b] = readPair(o);
                // a must come from an alpha set, b from a beta set —
                // and from the SAME epoch family (owner or foreign).
                const aOwner = inSet(a, ALPHA_BASE), aForeign = inSet(a, FOREIGN_ALPHA);
                const bOwner = inSet(b, BETA_BASE), bForeign = inSet(b, FOREIGN_BETA);
                if (!(aOwner || aForeign) || !(bOwner || bForeign)) {
                    print("FAILURE: cross-sentinel/torn read after poll-resume: alpha=" + a + " beta=" + b);
                    Atomics.store(gate, "stop", 1);
                    throw new Error("MC-AINT S4 / SPEC-jit I21(b) violated: alpha=" + a + " beta=" + b);
                }
                ++checks;
            }
            if (lastRound !== Atomics.load(gate, "done")) {
                lastRound = Atomics.load(gate, "done");
                Atomics.add(gate, "ready", 1); // round heartbeat
            }
        }
        return checks;
    }));
}

// Foreign mutator: performs the FIRST foreign write (synchronous
// writeThreadLocal fire => SW set => readers' E2-elided code jettisoned
// under the fire's stop) and then grows the object (foreign adds =>
// transition fires + out-of-line growth/segmentation) while readers run.
const foreign = new Thread(() => {
    let rounds = 0;
    let seen = -1;
    while (Atomics.load(gate, "stop") === 0) {
        const round = Atomics.load(gate, "go");
        if (round === seen || box.o === null) continue;
        seen = round;
        const o = box.o;
        // First foreign write: fires writeThreadLocal (Class-A, synchronous
        // STWR) while readers are mid-loop => they park at CheckTraps polls.
        o.alpha = FOREIGN_ALPHA + round;
        o.beta = FOREIGN_BETA + round;
        // Foreign growth: transition fires + butterfly reallocation /
        // segmentation right behind the resume.
        for (let i = 0; i < 24; ++i)
            o["grown" + round + "_" + i] = FOREIGN_ALPHA + round;
        // Re-assert sentinels after growth (offsets may have moved; elided
        // stale readers at old offsets now face grown storage).
        o.alpha = FOREIGN_ALPHA + round;
        o.beta = FOREIGN_BETA + round;
        Atomics.add(gate, "done", 1);
        ++rounds;
    }
    return rounds;
});

// Driver (owner of each round's structure family): build hot, signal, churn.
for (let round = 0; round < ROUNDS && Atomics.load(gate, "stop") === 0; ++round) {
    const o = freshTarget(round);
    // Warm the readers' compiled code shape on the owner thread's structure
    // family (TTL sets valid+watched at compile time).
    for (let i = 0; i < 1000; ++i)
        readPair(o);
    box.o = o;
    Atomics.store(gate, "go", round + 1);
    // Let readers + foreign mutator collide on this round.
    for (let i = 0; i < 20000; ++i) {
        // Owner-side benign rewrites within the owner sentinel set keep the
        // read loop's values moving without leaving the ALPHA/BETA sets.
        o.alpha = ALPHA_BASE + round;
        o.beta = BETA_BASE + round;
    }
}

Atomics.store(gate, "stop", 1);
const counts = joinAll(readers);
const foreignRounds = foreign.join();
for (const c of counts)
    shouldBeTrue(c > 0);
shouldBeTrue(foreignRounds > 0);
print("mc-aint-poll-resume-stale-elided: PASS (" + counts.join(",") + " checks; " + foreignRounds + " foreign rounds)");
