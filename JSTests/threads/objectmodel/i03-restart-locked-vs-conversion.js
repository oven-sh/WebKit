//@ requireOptions("--useJSThreads=1")
// Annex §15.T12 suite: locked-transition-vs-planned-conversion RESTART
// (§4.2-3 / §4.3-3; I10b/I11).
//
// Two foreign threads transition the SAME owner-created object concurrently.
// Each plans against a structureID it read before locking; whichever
// publishes second finds structureID != planning-time source under the cell
// lock and must RESTART the whole operation from §2 dispatch (fresh tag,
// fresh target structure, fresh F1/F2 checks) — not patch its stale plan.
// I10b: the TTL watchpoint fires precede the lock acquisition; I11: once
// fired, no lock-free foreign transition slips through. Observable: with K
// foreign threads each adding M disjoint properties, ALL K*M adds land with
// their values, and the object's shape is exactly the union — a botched
// RESTART loses an add (planned offset reused) or duplicates a key.
load("../harness.js", "caller relative");

const THREADS = 4;
const PER = 16;
const ROUNDS = 12;

for (let round = 0; round < ROUNDS; ++round) {
    // Owner-created object with a butterfly already installed (out-of-line
    // props), so foreign transitions hit the conversion + locked-transition
    // paths rather than the N3 install.
    const o = { a: 1 };
    for (let i = 0; i < 8; ++i)
        o["base" + i] = i;

    const workers = spawnN(THREADS, (t) => {
        // Tight add loops maximize planned-vs-published divergence: each add
        // plans a target structure, then races K-1 other planners.
        for (let m = 0; m < PER; ++m)
            o["r" + round + "_t" + t + "_m" + m] = (t << 8) | m;
        return true;
    });
    joinAll(workers).forEach((r, t) => shouldBeTrue(r, "worker " + t));

    for (let i = 0; i < 8; ++i)
        shouldBe(o["base" + i], i, "round " + round + ": pre-conversion property lost");
    for (let t = 0; t < THREADS; ++t) {
        for (let m = 0; m < PER; ++m) {
            const name = "r" + round + "_t" + t + "_m" + m;
            shouldBeTrue(name in o, "round " + round + ": RESTART lost add " + name);
            shouldBe(o[name], (t << 8) | m, "round " + round + ": RESTART aliased value of " + name);
        }
    }
    // Exactly the union — no duplicated keys from a double-published plan.
    shouldBe(Object.keys(o).length, 1 + 8 + THREADS * PER, "round " + round + ": shape not the exact union");
}

// Same race where one rival is a CONVERTER-by-element (array side: §4.2 via
// indexed growth) and the other a named-property transitioner: the two
// distinct §4.2/§4.3 entry points must still serialize via RESTART.
for (let round = 0; round < ROUNDS; ++round) {
    const a = [1, 2, 3, 4];

    const elementGrower = new Thread(() => {
        for (let i = 4; i < 200; ++i)
            a[i] = i;
        return true;
    });
    const propAdder = new Thread(() => {
        for (let m = 0; m < PER; ++m)
            a["p" + m] = "pv" + m;
        return true;
    });

    shouldBeTrue(elementGrower.join());
    shouldBeTrue(propAdder.join());

    shouldBe(a.length, 200, "round " + round);
    for (let i = 0; i < 200; ++i)
        shouldBe(a[i], i < 4 ? i + 1 : i, "round " + round + ": element lost across mixed RESTART");
    for (let m = 0; m < PER; ++m)
        shouldBe(a["p" + m], "pv" + m, "round " + round + ": named add lost across mixed RESTART");
}
