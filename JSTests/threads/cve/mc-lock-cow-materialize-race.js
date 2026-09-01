//@ requireOptions("--useJSThreads=1")
// MC-LOCK S4 (docs/threads/cve/map-MC-LOCK.md): CopyOnWrite materialization
// state-machine race — the Dirty COW (CVE-2016-5195) analog. The CoW break is
// a multi-step transition (allocate copy -> nuke-CAS -> publish private flat)
// serialized on the cell lock with the casButterfly expected pinned to the
// exact CoW word (SPEC-objectmodel §4.8/I35,
// ConcurrentButterfly.cpp tryMaterializeCopyOnWriteButterflyForSharedWrite).
// The historical round-3 bug was the OWNER's convertFromCopyOnWrite plain-nuke
// racing the locked foreign materializer — exactly the "revoker races the
// bias owner" shape. This storm races owner and foreign first-writes on the
// same CoW array, with CoW SIBLINGS from the same allocation site as the
// Dirty-COW oracle: a write that lands in the shared JSImmutableButterfly
// (skipped/torn break) becomes visible through a sibling.
//
// Oracle:
//  - siblings NEVER observe any write (shared copy never mutated);
//  - the raced element holds exactly one of the two writers' sentinels or
//    (only at indexes nobody wrote) the literal value;
//  - both writers' values survive at their disjoint indexes (no lost store
//    across the break, I21);
//  - no crash / RELEASE_ASSERT (I35 word-stability traps are part of the
//    protocol, not legal-program outcomes).
//
// EXECUTED POST-UNGIL ONLY. Amplifier-ready: tighten with the race amplifier
// at the cell-lock acquire and casButterfly hooks.
load("../harness.js", "caller relative");

const ROUNDS = 2000;
const OWNER_SENT = 0x0a11ce;
const FOREIGN_SENT = 0x0b0b00;

// Single allocation site => CoW butterfly shareable across calls.
function mk() { return [11, 22, 33, 44]; }

const gate = { round: 0, fdone: 0, stop: 0 };
const channel = { target: null };

const foreign = new Thread(() => {
    let seen = 0;
    while (Atomics.load(gate, "stop") === 0) {
        const r = Atomics.load(gate, "round");
        if (r === seen) {
            Atomics.wait(gate, "round", seen, 1);
            continue;
        }
        seen = r;
        const a = channel.target;
        // Foreign first write: must materialize a private flat copy first
        // (I35 cell-locked break), then store. Index 1 is foreign's lane.
        a[1] = FOREIGN_SENT;
        Atomics.store(gate, "fdone", seen);
        Atomics.notify(gate, "fdone");
    }
    return seen;
});

for (let r = 1; r <= ROUNDS; ++r) {
    const target = mk();
    const sibling1 = mk();
    const sibling2 = mk();
    channel.target = target;
    Atomics.store(gate, "round", r);
    Atomics.notify(gate, "round");

    // Owner first write races the foreign materializer on the SAME CoW word.
    // Index 2 is the owner's lane (disjoint from foreign's index 1, so JS
    // semantics require BOTH to survive).
    target[2] = OWNER_SENT;

    while (Atomics.load(gate, "fdone") !== r)
        Atomics.wait(gate, "fdone", Atomics.load(gate, "fdone"), 1);

    // --- Dirty COW oracle: the shared copy was never written. ---
    if (sibling1[1] !== 22 || sibling1[2] !== 33
        || sibling2[1] !== 22 || sibling2[2] !== 33) {
        throw new Error("round " + r + ": CoW sibling observed a write "
            + "(shared JSImmutableButterfly mutated): ["
            + sibling1 + "] / [" + sibling2 + "]");
    }
    // --- Lost-store oracle (I21): both disjoint writes survive the break. ---
    if (target[1] !== FOREIGN_SENT)
        throw new Error("round " + r + ": foreign write lost across CoW break: " + target[1]);
    if (target[2] !== OWNER_SENT)
        throw new Error("round " + r + ": owner write lost across CoW break: " + target[2]);
    // Untouched lanes keep literal values.
    if (target[0] !== 11 || target[3] !== 44)
        throw new Error("round " + r + ": untouched lane corrupted: [" + target + "]");
}

Atomics.store(gate, "stop", 1);
Atomics.store(gate, "round", ROUNDS + 1); // unblock a waiter mid-park
Atomics.notify(gate, "round");
shouldBe(foreign.join(), ROUNDS);
