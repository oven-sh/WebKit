//@ requireOptions("--useJSThreads=1")
// Annex §15.T12 suite: CoW — two foreign writers race materialization (I35).
//
// CoW payloads point at a shared JSImmutableButterfly. §4.8: any foreign
// write FIRST materializes a private flat butterfly via
// convertFromCopyOnWrite and casButterfly's it with the CoW-tagged word as
// expected; the LOSER of that CAS is a racing materializer and must
// re-dispatch onto the winner's butterfly (never publish its own copy — that
// would discard the winner's already-landed store). I35: no SW=1/segmented
// word ever points at the immutable butterfly, so sibling literals sharing
// it must NEVER observe either thread's write.
load("../harness.js", "caller relative");

const ROUNDS = 16;

for (let round = 0; round < ROUNDS; ++round) {
    const shared = [10, 20, 30, 40]; // CoW literal
    const sibling = [10, 20, 30, 40]; // same shape; shares the CoW pool's immutability guarantees

    // Two foreign writers race the materialization on DISJOINT indexes: if
    // the CAS loser wrongly publishes its own copy, the winner's disjoint
    // store vanishes.
    const w0 = new Thread(() => { shared[0] = "w0-" + round; return true; });
    const w1 = new Thread(() => { shared[3] = "w1-" + round; return true; });
    shouldBeTrue(w0.join());
    shouldBeTrue(w1.join());

    shouldBe(shared[0], "w0-" + round, "round " + round + ": winner's store discarded by racing materializer (I35)");
    shouldBe(shared[3], "w1-" + round, "round " + round + ": second writer lost (I35)");
    shouldBe(shared[1], 20, "round " + round + ": untouched CoW element corrupted");
    shouldBe(shared[2], 30, "round " + round);
    shouldBe(shared.length, 4);

    // The sibling literal must be bit-identical to its creation state: a
    // leaked write through the shared immutable butterfly is the §4.8
    // corruption I35 forbids.
    shouldBe(sibling[0], 10, "round " + round + ": foreign write leaked into shared immutable butterfly (I35)");
    shouldBe(sibling[1], 20);
    shouldBe(sibling[2], 30);
    shouldBe(sibling[3], 40);
}

// Same-index flavor: both writers hit index 0; the final value must be one
// of the two (the CAS loser re-dispatches onto the winner's flat butterfly
// and its plain store then races normally — either order is legal, nothing
// else is).
for (let round = 0; round < ROUNDS; ++round) {
    const shared = ["a", "b", "c"];
    const w0 = new Thread(() => { shared[0] = "first"; });
    const w1 = new Thread(() => { shared[0] = "second"; });
    w0.join();
    w1.join();
    const v = shared[0];
    if (v !== "first" && v !== "second")
        throw new Error("round " + round + ": same-index CoW race produced " + describe(v));
    shouldBe(shared[1], "b");
    shouldBe(shared[2], "c");
}

// CoW double literal: materialization + R-DOUBLE (§4.7) — raw 8-byte double
// slots, no boxing. Values must round-trip exactly.
for (let round = 0; round < ROUNDS; ++round) {
    const d = [0.5, 1.5, 2.5];
    const w = new Thread(() => { d[1] = 99.875; return d[0]; });
    shouldBe(w.join(), 0.5, "round " + round + ": CoW double read through materialization");
    shouldBe(d[1], 99.875, "round " + round + ": CoW double write lost");
    shouldBe(d[0], 0.5);
    shouldBe(d[2], 2.5);
}
