//@ requireOptions("--useJSThreads=1")
// MC-CODE S3 (docs/threads/cve/map-MC-CODE.md): i-cache/patch-ordering for a
// thread that sleeps ACCESS-RELEASED through a code-patching stop — the
// AArch64 cross-modifying-code exemplar (hotspot-cmc / deopt-trap class).
//
// jit R1.d's per-mutator ISB fires only on NVS exit; a thread parked in
// Atomics.wait (heap access released) never executes that exit for stops it
// slept through. UNGIL ANNEX ISB1 (§A.3.2c; runtime/VMLite.cpp
// jsThreadsSyncToStopGenerationBeforeJITEntry) closes the hole: every
// non-NVS may-execute-JIT transition compares the process-wide
// stop-generation counter and issues an ISB on mismatch BEFORE re-entering
// JIT code. This is the chartered-but-unwritten U-T5 arm 6 exercise.
//
// Shape: a sleeper thread warms a hot function whose optimized body elides a
// prototype-property load behind a watchpoint (replace-watchpoint on the
// proto structure), then parks in a property Atomics.wait. The main thread
// then MUTATES the watched fact (proto.f = new value) — a Class-A fire =>
// STW => jettison of the sleeper's optimized CodeBlock + patching — and only
// THEN notifies the sleeper. The sleeper wakes through the park-site path
// (NOT an NVS exit for the fire's stop on the arm64 failure mode), re-enters
// the (re)compiled code and must observe the NEW value. A stale instruction
// stream returns the old constant-folded value or crashes.
//
// Oracle is deterministic at the JS level: after notify, every call must
// return the round's new value. The underlying ISB omission is only
// PHYSICALLY observable on arm64 multi-core (stale i-cache lines), so this
// test is both a correctness check everywhere and the amplifier arm's
// skeleton for arm64 hardware (run many rounds, pin threads to distinct
// cores). EXECUTED POST-UNGIL ONLY.
load("../harness.js", "caller relative");

const ROUNDS = 200;
const WARM = 5000;

const gate = { warmed: 0, round: 0, ack: 0 };
const proto = { f: 1 };
const obj = Object.create(proto);
obj.pad = 0;

const sleeper = new Thread(() => {
    function hot(o) {
        // Proto-chain load: DFG/FTL elides the proto re-check behind the
        // replace/structure watchpoint on `proto`. Mutating proto.f fires it.
        return o.f + 1;
    }
    noInline(hot);
    let sum = 0;
    for (let i = 0; i < WARM; ++i)
        sum += hot(obj);
    if (sum !== WARM * 2)
        throw new Error("warmup sum wrong: " + sum);
    Atomics.store(gate, "warmed", 1);
    Atomics.notify(gate, "warmed");

    for (let r = 1; r <= ROUNDS; ++r) {
        // Park access-released until the main thread has patched code for
        // round r. The fire/jettison happens WHILE we are parked here.
        while (Atomics.load(gate, "round") < r)
            Atomics.wait(gate, "round", r - 1, 50);
        const expect = (r + 1) + 1; // proto.f === r + 1 after round r's mutation
        // First re-entry into JIT code after waking: must run CURRENT code
        // against the CURRENT fact. Stale i-cache => old constant / crash.
        for (let i = 0; i < 64; ++i) {
            const got = hot(obj);
            if (got !== expect)
                throw new Error("round " + r + ": hot() returned " + got + ", expected " + expect + " — executed stale code after sleeping through the jettison stop");
        }
        Atomics.add(gate, "ack", 1);
    }
    return true;
});

waitUntil(() => Atomics.load(gate, "warmed") === 1);

for (let r = 1; r <= ROUNDS; ++r) {
    // Class-A fire + jettison of the sleeper's optimized code while the
    // sleeper is parked: replace-watchpoint on proto's structure fires on
    // the value change; the sleeper's DFG/FTL hot() jettisons inside the
    // stop (SPEC-jit §5.3/§5.6), code is patched, stop-generation bumps
    // (ANNEX ISB1.1), world resumes — all before the sleeper is notified.
    proto.f = r + 1;
    Atomics.store(gate, "round", r);
    Atomics.notify(gate, "round");
    waitUntil(() => Atomics.load(gate, "ack") === r);
}

shouldBeTrue(sleeper.join());
print("mc-code-sleep-through-jettison-isb: PASS (" + ROUNDS + " sleep-through-patch rounds)");
