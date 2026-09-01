//@ requireOptions("--useJSThreads=1", "--useThreadGILOffUnsafe=1", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--thresholdForJITAfterWarmUp=20", "--thresholdForOptimizeAfterWarmUp=100", "--watchdog=120000", "--watchdog-exception-ok")
// MC-SAFE S3 (docs/threads/cve/map-MC-SAFE.md): unbounded time-to-safepoint
// in a poll-free native region (Yarr) => 30s stop-watchdog fail-stop.
//
// SUSCEPTIBILITY DEMONSTRATOR — EXPECTED TO FAIL-STOP (RELEASE_ASSERT in
// JSThreadsSafepoint::watchdogAssertStopProgress, JSThreadsSafepoint.cpp:
// 401-413, reached from the §A.3 conductor predicate loop, VMManager.cpp:
// 594) on a tree where Yarr has no D9-quantum stop/termination poll
// (Source/JavaScriptCore/yarr/ has zero VMTraps references). It PASSES once
// Yarr gains a backtrack-budget poll that release-access-parks per the
// §A.3.2b protocol.
//
// Mechanism: thread A enters a catastrophic-backtracking regexp match,
// calibrated below to run ~90s, holding its client heap access the whole
// time (no poll site inside Yarr). The main thread then fires a Class-A
// jettison; the §A.3 conductor's access-based predicate (§A.3.2) cannot
// converge while A holds access; at 30s the stop watchdog converts the
// stall into a deterministic whole-process crash. Availability only — the
// conductor patches nothing before convergence (fail-closed) — but it is a
// remote DoS primitive for any threads-enabled embedder: one regexp plus
// any stop requester. Note the same gap blocks VM-wide TERMINATION delivery
// into Yarr (§A.2 rule 4), so the runaway regexp cannot be killed either;
// the outer --watchdog=120000 only fires after the regexp returns or the
// engine gains the poll.
//
// PASS criterion (post-fix): the jettison's stop completes in < 25s.
//
// EXECUTED POST-UNGIL ONLY. Run this test LAST / isolated: in the
// susceptible state it takes ~32s to crash; in the fixed state ~tens of
// seconds bounded by the calibrated regexp + watchdog termination.
load("../harness.js", "caller relative");

const gate = { started: 0, calibratedN: 0 };

const nowMs = (typeof preciseTime === "function") ? () => preciseTime() * 1000 : () => Date.now();

// Calibrate on the MAIN thread first: /^(a+)+$/ against "a".repeat(n) + "!"
// roughly doubles per added 'a'. Find n where one match costs ~40-80ms,
// then project to ~90s (about 11 doublings). Clamp hard so a calibration
// mishap cannot pick a multi-hour run.
function matchCost(n) {
    const s = "a".repeat(n) + "!";
    const re = /^(a+)+$/;
    const t0 = nowMs();
    re.test(s);
    return nowMs() - t0;
}

let n = 12;
let cost = matchCost(n);
while (cost < 40 && n < 40) {
    ++n;
    cost = matchCost(n);
}
let target = n;
let projected = cost;
while (projected < 90000 && target < n + 14) {
    ++target;
    projected *= 2;
}
shouldBeTrue(target > n, "calibration projected a longer run");

const worker = new Thread(() => {
    waitUntil(() => Atomics.load(gate, "calibratedN") !== 0);
    const len = Atomics.load(gate, "calibratedN");
    Atomics.add(gate, "started", 1);
    // ~90s of poll-free Yarr backtracking, heap access held throughout.
    const s = "a".repeat(len) + "!";
    return /^(a+)+$/.test(s);
});

// Victim for the Class-A jettison (same shape as mc-safe-spin-vs-classa-stop).
const proto = { y: 1 };
const o = Object.create(proto);
const f = Function("o", "return o.y + 1;");
for (let i = 0; i < 2000; ++i)
    f(o);

Atomics.store(gate, "calibratedN", target);
waitUntil(() => Atomics.load(gate, "started") === 1);
sleepMs(2000); // Let the worker get deep into the match.

const t0 = nowMs();
proto.y = 2; // Class-A fire => jettison => §A.3 stop request.
const elapsed = nowMs() - t0;

// Susceptible tree: we never get here — the process RELEASE_ASSERTed at
// ~30s inside watchdogAssertStopProgress with a nil Class-A context (the
// requester is a jettison).
shouldBeTrue(elapsed < 25000, "Class-A stop converged against an in-flight Yarr match (took " + elapsed + "ms)");
shouldBe(f(o), 3);
// Do NOT join the worker: its regexp may legitimately run for the rest of
// the calibrated budget. The VM watchdog (--watchdog=120000) bounds the
// process; --watchdog-exception-ok makes that exit acceptable.
