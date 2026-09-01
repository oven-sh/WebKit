//@ requireOptions("--useJSThreads=1", "--useThreadGILOffUnsafe=1", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--thresholdForJITAfterWarmUp=20", "--thresholdForOptimizeAfterWarmUp=100")
// MC-SAFE S1+S2 (docs/threads/cve/map-MC-SAFE.md): safepoint reachability
// liveness for pure-JS spinning siblings, and the trap-bit consumption race.
//
// S1: the spinner threads below have ONLY loop-hint polls available
// (BytecodeGenerator emits OpLoopHint+OpCheckTraps on every back edge;
// useJSThreads forces usePollingTraps=1, Options.cpp:917-920) — every
// Class-A stop the main thread requests must still converge.
//
// S2: under the §A.2.1 interim seam the per-lite stop bits alias ONE VM-wide
// trap word and VMTraps' take rule clears NeedStopTheWorld at the FIRST
// trapping thread. With N>=2 spinners, sibling 2..N would never trap after
// sibling 1 consumed the bit — the §A.3 conductor re-fires requestStop() on
// every non-quiescent predicate sample (VMManager.cpp:583-594) exactly for
// this. That re-fire is marked "RETIRED when the per-lite trap words land":
// this test is the regression guard across that migration. If either
// mechanism regresses, the conductor predicate hangs and the 30s stop
// watchdog (JSThreadsSafepoint.cpp:401-413) RELEASE_ASSERTs => this test
// fails by crash instead of by assertion.
//
// Class-A stop trigger: warm a fresh DFG-compiled function whose fast path
// folds a prototype property load under a replacement watchpoint, then
// replace that property — the fire jettisons the CodeBlock, and every
// flag-on jettison (reason != OldAge) routes through
// JSThreadsSafepoint::stopTheWorldAndRun (SPEC-jit §5.3 choke point), which
// GIL-off takes the real §A.3 thread-granular conductor.
//
// EXECUTED POST-UNGIL ONLY. Deterministic pass criterion: all ROUNDS stops
// complete well under the 30s watchdog while SPINNERS siblings burn JS.
load("../harness.js", "caller relative");

const SPINNERS = 3;
const ROUNDS = 20;
const gate = { started: 0, stop: 0 };

const spinners = spawnN(SPINNERS, () => {
    Atomics.add(gate, "started", 1);
    let acc = 0;
    // Hot pure-JS loop: the only safepoint polls on this thread are the
    // per-back-edge OpCheckTraps. The Atomics.load is itself a native call,
    // so keep it infrequent — the inner loop is poll-via-loop-hint only.
    while (Atomics.load(gate, "stop") === 0) {
        for (let i = 0; i < 100000; ++i)
            acc = (acc + i) | 0;
    }
    return acc | 1;
});

waitUntil(() => Atomics.load(gate, "started") === SPINNERS);

function buildVictim(round) {
    // Fresh prototype + fresh function source per round so each round gets
    // its own CodeBlock and its own un-fired replacement watchpoint.
    const proto = { y: 1 };
    const o = Object.create(proto);
    const f = Function("o", "/* round " + round + " */ return o.y + 1;");
    for (let i = 0; i < 2000; ++i)
        f(o); // Tier up; the DFG load of proto.y installs the watchpoint.
    return { proto, o, f };
}

const nowMs = (typeof preciseTime === "function") ? () => preciseTime() * 1000 : () => Date.now();

let slowestMs = 0;
for (let r = 0; r < ROUNDS; ++r) {
    const { proto, o, f } = buildVictim(r);
    const t0 = nowMs();
    proto.y = 2 + r; // Replacement => watchpoint fire => jettison => §A.3 stop.
    const t1 = nowMs();
    const ms = t1 - t0;
    if (ms > slowestMs)
        slowestMs = ms;
    shouldBe(f(o), 3 + r); // Post-stop sanity: re-execution sees the new value.
    // The watchdog fail-stop is at 30000ms; anything in that order of
    // magnitude means stop delivery to the spinners is broken even if it
    // eventually converged.
    shouldBeTrue(ms < 20000, "round " + r + " stop converged (took " + ms + "ms)");
}

Atomics.store(gate, "stop", 1);
const results = joinAll(spinners);
for (const v of results)
    shouldBeTrue(v !== 0, "spinner made progress");
