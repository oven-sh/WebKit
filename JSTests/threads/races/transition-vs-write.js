//@ requireOptions("--useJSThreads=1")
// API-I16 (GI; amplifier+TSAN when present, G15): shape transitions racing
// WRITES on one shared object. Targets the THREAD.md numbered invariants
// from the writer side:
//   - no lost properties: N threads adding disjoint property ranges to the
//     same object must all land — racing transitions may not drop a
//     concurrent add (THREAD.md regime-2/3 soundness);
//   - no lost Atomics-published stores: a shared SeqCst counter advanced by
//     every writer is exact;
//   - same-name racing plain writes: the final value is one of the written
//     values, never a torn or invented one;
//   - delete quarantine: a property deleted and re-added under racing
//     traffic never aliases stale data — readers/writers see only
//     {old value, absent, new value} (THREAD.md regime 3: deleted slots are
//     quarantined until a safepoint so stale readers can't alias a newly
//     added property).
//
// Deterministically green under the phase-1 GIL; designed for
// Tools/threads/amplify.sh. Annex T2: bounded blocking; all threads joined.
load("../harness.js", "caller relative");

const N = 4;     // writer threads
const PER = 250; // properties added per writer
const ROUNDS = 200;
const o = { seq: 0, shared: "tag-main", churn: 0 };
const gate = { started: 0, go: 0 };

const writers = spawnN(N, which => {
    Atomics.add(gate, "started", 1);
    while (Atomics.load(gate, "go") === 0)
        Atomics.wait(gate, "go", 0, 100);
    for (let i = 0; i < PER; ++i) {
        const n = which * PER + i;
        o["w" + n] = n;            // racing transition adds (disjoint names)
        Atomics.add(o, "seq", 1);  // SeqCst published counter
        o.shared = "tag" + which;  // same-name racing plain writes
        // delete-quarantine probe from the writer side: the churn slot must
        // read as one of the values main ever wrote to it (a number or a
        // {round} box) or be absent mid-delete — never garbage.
        const c = o.churn;
        if (!(c === undefined || typeof c === "number"
            || (typeof c === "object" && c !== null && typeof c.round === "number")))
            throw new Error("churn aliased an unwritten value: " + String(c));
    }
    return "done";
});

waitUntil(() => Atomics.load(gate, "started") === N);
Atomics.store(gate, "go", 1);
Atomics.notify(gate, "go", Infinity);

// Main: transition churn + delete/re-add cycles on the SAME object while
// the writers hammer it.
for (let round = 0; round < ROUNDS; ++round) {
    o["m" + round] = round;            // more racing transitions
    o.churn = { round };               // boxed value (heap pointer: an alias bug surfaces as a wrong object)
    if (o.churn.round !== round)
        throw new Error("churn box lost its payload in round " + round);
    delete o.churn;                    // delete: slot quarantined
    if (o.churn !== undefined)
        throw new Error("delete failed in round " + round);
    o.churn = round;                   // re-add with a different representation
    if (o.churn !== round)
        throw new Error("re-added churn lost: " + o.churn + " in round " + round);
    if (round % 16 === 0)
        sleepMs(1);                    // let writers interleave
}

shouldBe(joinAll(writers).join(","), "done,done,done,done");

// no lost properties: every writer's complete range, exact values
for (let w = 0; w < N; ++w) {
    for (let i = 0; i < PER; ++i) {
        const n = w * PER + i;
        shouldBe(o["w" + n], n);
    }
}
// main's transitions all landed too
for (let round = 0; round < ROUNDS; ++round)
    shouldBe(o["m" + round], round);
// no lost Atomics-published stores
shouldBe(o.seq, N * PER);
// same-name race: final value is ONE OF the written values
shouldBeTrue(/^tag([0-3]|-main)$/.test(o.shared), "o.shared must be a written value, got " + o.shared);
// churn ends at the last re-add
shouldBe(o.churn, ROUNDS - 1);

// AB17e O2/GT11 proto-chain-dictionary arm: hot IC-cached gets THROUGH a
// dictionary prototype from multiple threads. Before the sibling-site gates
// (Repatch.cpp tryCacheGetBy unset/proto arm, prepareChainForCaching,
// normalizePrototypeChain) this shape requested a §10.6 per-event stop while
// holding codeBlock->m_lock — a deterministic 30s watchdog wedge. The arm is
// green iff it completes (no wedge) with exact values; the self-property
// dictionary path is covered by the actionForCell gate and the rounds above.
{
    const proto = {};
    for (let i = 0; i < 200; ++i)
        proto["pp" + i] = i;
    delete proto.pp0; // dictionary + churn => stays dictionary, unflattened
    const child = Object.create(proto);
    child.own = 7;
    const pgate = { go: 0 };
    const readers = spawnN(N, which => {
        while (Atomics.load(pgate, "go") === 0)
            Atomics.wait(pgate, "go", 0, 100);
        let sum = 0;
        for (let i = 0; i < 50000; ++i)
            sum += child.pp5 + child.own; // proto-hit get: IC attempt meets a proto-chain dictionary
        return sum;
    });
    Atomics.store(pgate, "go", 1);
    Atomics.notify(pgate, "go", Infinity);
    let mainSum = 0;
    for (let i = 0; i < 50000; ++i)
        mainSum += child.pp5 + child.own;
    shouldBe(mainSum, 50000 * 12);
    shouldBe(joinAll(readers).join(","), Array(N).fill(50000 * 12).join(","));
}
