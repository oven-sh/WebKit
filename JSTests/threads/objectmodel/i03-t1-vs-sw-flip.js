//@ requireOptions("--useJSThreads=1")
// Annex §15.T12 suite: T1-vs-SW-flip (I27).
//
// T1 is the lock-free COPYING flat element resize: legal ONLY with expected
// tag (currentTID, 0), and an SW DCAS must fail that CAS (the resize then
// retries as T2, publishing (notTTLTID, 1) — never copying elements from a
// non-(currentTID,0) butterfly outside STW, I27). Scenario: the OWNER (main
// thread) grows a dense array through repeated copying resizes while a
// foreign thread writes existing elements (the F1 SW flip). Invariants:
//   - no grown element is lost (the T1-loser re-dispatch must re-copy or go
//     segmented, never publish a stale copy missing the foreign store);
//   - the foreign writes land exactly once each (a winning SW write must not
//     be undone by a racing element-storage copy);
//   - length is exact.
load("../harness.js", "caller relative");

const ROUNDS = 8;
const GROW_TO = 600;        // enough appends to cross several vectorLength doublings
const FOREIGN_WRITES = 64;

for (let round = 0; round < ROUNDS; ++round) {
    const a = [0]; // owner-installed flat butterfly, tag (main, 0)
    for (let i = 1; i < 8; ++i)
        a[i] = i; // the foreign window, owner-written BEFORE the gate opens
    const gate = { go: 0 };

    const writer = new Thread(() => {
        while (Atomics.load(gate, "go") === 0)
            sleepMs(1);
        // Foreign writes to ALREADY-EXISTING low indexes: SW flip + plain
        // stores; raced against the owner's copying growth.
        for (let w = 0; w < FOREIGN_WRITES; ++w) {
            a[w % 8] = "foreign" + w;
            if (!(w & 7))
                sleepMs(1); // let the owner's resize loop interleave
        }
        return true;
    });

    Atomics.store(gate, "go", 1);
    for (let i = 8; i < GROW_TO; ++i) {
        a[i] = i; // owner append; resize path = T1 while (main,0), T2 after the flip
        if (!(i & 63))
            sleepMs(1);
    }
    shouldBeTrue(writer.join());

    shouldBe(a.length, GROW_TO, "round " + round + ": length after raced growth");
    // Owner appends above the foreign window are interleaving-independent.
    for (let i = 8; i < GROW_TO; ++i) {
        if (a[i] !== i)
            throw new Error("round " + round + ": lost owner element a[" + i + "] = " + describe(a[i]) + " (I27: stale copy published)");
    }
    // The foreign window: the foreign writer is the ONLY post-gate writer of
    // indexes 0..7 (the owner filled them before the gate opened), so each
    // slot's final value is exactly the last foreign write to it.
    for (let s = 0; s < 8; ++s) {
        const expected = "foreign" + (FOREIGN_WRITES - 8 + s);
        shouldBe(a[s], expected, "round " + round + ": foreign write lost in resize copy (I27)");
    }
}
