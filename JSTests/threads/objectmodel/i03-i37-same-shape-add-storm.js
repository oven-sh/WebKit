//@ requireOptions("--useJSThreads=1")
// Annex §15.T12 addendum (r14) suite: I37 same-shape add storm.
//
// N threads, ONE shared constructor shape, racing out-of-line adds on
// DISTINCT instances. Every thread walks the SAME transition chain
// (S -> S+f0 -> S+f0+f1 -> ...), so this hammers L6: transition-table
// LOOKUPS under the source's m_lock (the Concurrently variant), inserts
// already m_lock-held, property-table steal/clone/materialize under the
// source's m_lock, and uncached table walks held across the walk. I37:
// no lost or DUPLICATED transitions (two threads racing the same missing
// edge must converge on one target structure) and no torn table reads —
// observable as: every instance ends with exactly the same shape, every
// property readable with its writer's value.
load("../harness.js", "caller relative");

const THREADS = 4;
const INSTANCES_PER_THREAD = 8;
const ADDS = 24; // same names, same order, on every instance
const ROUNDS = 6;

for (let round = 0; round < ROUNDS; ++round) {
    function Shared() { // fresh shape per round (closure identity)
        this.x = 0;
        this.y = 1;
    }
    // Prime: ONE main-thread instance walks nothing — the chain is built
    // entirely by the racing threads below.
    const all = [];

    const workers = spawnN(THREADS, (t) => {
        const mine = [];
        for (let n = 0; n < INSTANCES_PER_THREAD; ++n) {
            const o = new Shared();
            for (let k = 0; k < ADDS; ++k)
                o["storm" + k] = t * 10000 + n * 100 + k; // same-shape add: races the SAME table edge
            mine.push(o);
            if (!(n & 3))
                sleepMs(1); // interleave chain walking across threads
        }
        return mine;
    });
    const results = joinAll(workers);
    results.forEach(mine => all.push(...mine));

    shouldBe(all.length, THREADS * INSTANCES_PER_THREAD);
    for (let t = 0; t < THREADS; ++t) {
        const mine = results[t];
        for (let n = 0; n < INSTANCES_PER_THREAD; ++n) {
            const o = mine[n];
            shouldBe(o.x, 0, "round " + round + ": inline x");
            shouldBe(o.y, 1, "round " + round + ": inline y");
            for (let k = 0; k < ADDS; ++k)
                shouldBe(o["storm" + k], t * 10000 + n * 100 + k,
                    "round " + round + " t" + t + " n" + n + ": lost/aliased add storm" + k + " (I37)");
            // Same shape across every instance: identical key sets in
            // identical order (a duplicated transition would fork the chain
            // and CAN diverge in enumeration order or count).
            const keys = Object.keys(o);
            shouldBe(keys.length, 2 + ADDS, "round " + round + ": shape forked (I37)");
            shouldBe(keys[0], "x");
            shouldBe(keys[1], "y");
            for (let k = 0; k < ADDS; ++k)
                shouldBe(keys[2 + k], "storm" + k, "round " + round + ": transition order diverged (I37)");
        }
    }
}

// Deletion-edge flavor: racing REMOVE transitions over one shared shape on
// distinct instances (removePropertyTransition table lookups race the same
// way; quarantine applies to every deleted out-of-line offset).
for (let round = 0; round < ROUNDS; ++round) {
    function Shaped() {
        this.a = 1;
        this.b = 2;
    }
    const seeds = [];
    for (let i = 0; i < THREADS * 4; ++i) {
        const o = new Shaped();
        for (let k = 0; k < 12; ++k)
            o["del" + k] = k;
        seeds.push(o);
    }
    const workers = spawnN(THREADS, (t) => {
        for (let i = t * 4; i < (t + 1) * 4; ++i) {
            const o = seeds[i];
            for (let k = 0; k < 12; k += 2)
                delete o["del" + k];
        }
        return true;
    });
    joinAll(workers).forEach(r => shouldBeTrue(r));

    for (const o of seeds) {
        for (let k = 0; k < 12; ++k) {
            if (k % 2)
                shouldBe(o["del" + k], k, "round " + round + ": survivor lost in remove storm (I37)");
            else {
                shouldBeFalse("del" + k in o, "round " + round + ": remove lost (I37)");
                shouldBe(o["del" + k], undefined);
            }
        }
        shouldBe(o.a, 1);
        shouldBe(o.b, 2);
    }
}
