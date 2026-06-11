//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// watchpoint-storm.js — gc-stress suite: one storm thread repeatedly FIRES
// watchpoints (structure-transition and property-replacement family) on a
// shared prototype while N reader threads run the corresponding inline-cache
// fast paths.
//
// The storm thread cycles, on a prototype the readers' receivers inherit
// from:
//   - add/delete of a fresh property        (structure transitions; fires
//     transition watchpoints and invalidates prototype-chain ICs),
//   - replacement of an existing property   (fires the property's
//     replacement watchpoint / breaks constant inference),
//   - periodic dictionary round-trips via $vm.toCacheableDictionary /
//     flattenDictionaryObject when available (forces IC resets), and
//   - periodic $vm.gc() so watchpoint/IC teardown overlaps sweeping.
//
// Readers hammer two fast paths against the SAME chain the storm mutates:
//   own-property load (o.own) and prototype load (o.viaProto). Every loaded
//   value must be in the small expected domain — a fast path that keeps
//   running against retired watchpoint state can return a stale or torn
//   value outside the domain, or crash on a freed stub.
//
// Under the phase-1 GIL readers interleave cooperatively (sleepMs(0)
// yields); post-GIL the same file is a true concurrent storm. Value is
// amplified under gc-stress-matrix.sh modes (scribble/zombie make freed
// watchpoint/IC memory visibly poisoned; collectContinuously overlaps the
// fires with marking).
//
// Runtime: bounded — fixed 400 storm rounds; readers stop via an Atomics
// gate (corpus convention, annex T2: no unsynchronized cross-thread flags —
// keeps the TSAN rung free of incidental plumbing races and makes loop
// termination an ordered, not hoist-able, observation).

load("../harness.js", "caller relative");

const READERS = 3;
const STORM_ROUNDS = 400;

const PROTO_A = 1001;
const PROTO_B = 2002;
const OWN_VALUE = 7;
const EXPECTED_PROTO = new Set([PROTO_A, PROTO_B]);

const proto = { viaProto: PROTO_A, stableAnchor: 0 };

function makeReceiver() {
    const o = Object.create(proto);
    o.own = OWN_VALUE;
    o.ownTail = 0xdead; // neighbor poison: a torn offset reads this
    return o;
}

function readOwn(o) { return o.own; }
noInline(readOwn);
function readProto(o) { return o.viaProto; }
noInline(readProto);

// Warm the ICs on the stable receiver shape before the storm starts.
const stable = makeReceiver();
for (let i = 0; i < 10000; ++i) {
    if (readOwn(stable) !== OWN_VALUE || !EXPECTED_PROTO.has(readProto(stable)))
        throw new Error("warmup mismatch at " + i);
}

const gate = { started: 0, stop: 0 };

const readers = spawnN(READERS, function (index) {
    Atomics.add(gate, "started", 1);
    let reads = 0;
    const mine = makeReceiver();
    while (Atomics.load(gate, "stop") === 0) {
        const own1 = readOwn(stable);
        const own2 = readOwn(mine);
        const p1 = readProto(stable);
        const p2 = readProto(mine);
        if (own1 !== OWN_VALUE || own2 !== OWN_VALUE)
            throw new Error("reader " + index + ": own-load fast path returned " + own1 + "/" + own2 + " (expected " + OWN_VALUE + ")");
        if (!EXPECTED_PROTO.has(p1) || !EXPECTED_PROTO.has(p2))
            throw new Error("reader " + index + ": proto-load fast path returned " + p1 + "/" + p2 + " (outside {" + PROTO_A + "," + PROTO_B + "})");
        ++reads;
        if (!(reads % 256))
            sleepMs(0); // cooperative-GIL yield so the storm interleaves
    }
    // Exact-value epilogue (non-vacuity for the CROSS-THREAD stale-constant
    // case): the seq-cst stop store below is sequenced after the final
    // proto.viaProto = PROTO_B write of round 399, so once this thread has
    // observed stop === 1 the only legal proto value is exactly PROTO_B.
    // Inside the loop a per-thread fast path stuck on the pre-fire constant
    // PROTO_A is INSIDE the expected domain and invisible; here it is a
    // deterministic per-thread failure.
    const finalStable = readProto(stable);
    const finalMine = readProto(mine);
    if (finalStable !== PROTO_B || finalMine !== PROTO_B)
        throw new Error("reader " + index + ": post-stop proto reads "
            + describe(finalStable) + "/" + describe(finalMine)
            + " (expected exactly " + PROTO_B
            + "; this thread's fast path kept a stale pre-fire constant)");
    return reads;
});

// Started-rendezvous: every reader must be running before the fixed-bound
// storm begins, so the reads[i] > 0 assertions are deterministic instead of
// depending on thread-startup scheduling luck under the cooperative GIL.
waitUntil(() => Atomics.load(gate, "started") === READERS, 30000);

// Storm loop (runs on the main thread, which owns the GIL between yields).
const haveDollarVM = typeof $vm !== "undefined";
for (let round = 0; round < STORM_ROUNDS; ++round) {
    // Transition fires: add + delete a fresh property name each round so
    // the prototype's structure keeps transitioning (no cache settles).
    proto["storm" + (round & 31)] = round;
    delete proto["storm" + (round & 31)];

    // Replacement fires: flip the inherited property between two values in
    // the expected domain. Readers must only ever see A or B.
    const expectNow = (round & 1) ? PROTO_B : PROTO_A;
    proto.viaProto = expectNow;

    // Same-thread read-back: sequential semantics on this thread make this
    // DETERMINISTIC — a replacement watchpoint that fired without
    // invalidating a constant-folded/cached fast path keeps returning the
    // stale pre-fire value here, every round after the first flip.
    const back = readProto(stable);
    if (back !== expectNow)
        throw new Error("storm round " + round + ": same-thread read-back returned "
            + describe(back) + ", expected " + expectNow
            + " (stale constant after replacement watchpoint fire)");

    // Dictionary round-trip: forces IC resets against the cached chain.
    if (haveDollarVM && $vm.toCacheableDictionary && !(round % 25)) {
        $vm.toCacheableDictionary(proto);
        if (readProto(stable) !== expectNow)
            throw new Error("storm round " + round + ": post-dictionary proto read returned "
                + describe(readProto(stable)) + ", expected " + expectNow);
        if ($vm.flattenDictionaryObject)
            $vm.flattenDictionaryObject(proto);
    }

    // Overlap teardown with GC sweeping.
    if (haveDollarVM && !(round % 50))
        $vm.gc();

    if (!(round % 10))
        sleepMs(0); // drop the GIL so readers run mid-storm
}

Atomics.store(gate, "stop", 1);
Atomics.notify(gate, "stop", Infinity);
const reads = joinAll(readers);
for (let i = 0; i < reads.length; ++i)
    shouldBeTrue(reads[i] > 0, "reader " + i + " must have completed reads");

// Post-state coherence: the chain still answers correctly after the storm.
// The last storm round is STORM_ROUNDS-1 = 399 (odd), so the exact final
// value is PROTO_B — domain membership would let a stale constant slip by.
shouldBe(readOwn(stable), OWN_VALUE);
shouldBe(readProto(stable), PROTO_B, "final proto value must be the last-written value");
shouldBe(readOwn(makeReceiver()), OWN_VALUE);
shouldBe(readProto(makeReceiver()), PROTO_B, "fresh receiver must see the last-written proto value");

print("watchpoint-storm: PASS");

// WOULD-FAIL-IF: watchpoint fire/invalidation is not coherent across
// threads — e.g. a transition or replacement watchpoint fired by one thread
// retires IC/stub state while another thread is still executing that fast
// path (use-after-retire of a stub), or a fast path keeps returning the
// pre-fire constant after the watchpoint fired. The stale-constant variant
// is caught in BOTH placements: same-thread, deterministically, by the storm
// thread's read-back after every viaProto flip (sequential semantics: the
// just-written value must be observed) and by main's exact-final-value
// checks; cross-thread by each READER's post-stop exact-value epilogue —
// after observing the seq-cst stop store (which is sequenced after the final
// PROTO_B write) a reader whose per-thread IC/fast-path state was never
// invalidated still returns PROTO_A, which the epilogue rejects (the
// in-loop asserts alone could not: PROTO_A is inside the legal mid-race
// domain). The cross-thread torn/poison/UAF variants trip the readers'
// in-loop asserts: an own-load != 7, a proto-load outside {1001, 2002}
// (torn read of the neighbor poison 0xdead), or a crash inside the retired
// stub. The dictionary round-trips + $vm.gc() rounds make the freed-stub
// variant land in swept memory, which the matrix's scribble/zombie modes
// turn into a deterministic poison read.
