//@ requireOptions("--useJSThreads=1")
// AUD1.N4(3) (AB17i item 1): racing for-in enumerator-cache INSTALLS on
// shared Structures. N threads + main simultaneously for-in objects whose
// Structures have never had an enumerator cached, so every round each fresh
// Structure sees concurrent Structure::setCachedPropertyNameEnumerator
// installs. Before the locked install, one thread's FixedVector rebuild
// could free the StructureChainInvalidationWatchpoints another thread had
// just linked into live transition WatchpointSets (UAF on fire), and the
// enumerator/flag word could be torn. Green iff every thread enumerates
// exactly the expected keys/values every round.
//
// Deterministically green under the phase-1 GIL; designed for
// Tools/threads/amplify.sh. Annex T2: bounded blocking; all threads joined.
load("../harness.js", "caller relative");

const N = 4;       // enumerating threads (main makes N+1 racers)
const SHAPES = 8;  // distinct structures per round
const ROUNDS = 40;

// Shared cells: g.round advances 1..ROUNDS, then -1 to stop; g.list is the
// round's freshly-shaped objects; g.done counts threads finished with the
// current round.
const g = { round: 0, done: 0, list: null };

function buildList(r) {
    // Round-tagged property names => fresh transition tree => fresh
    // Structures whose enumerator caches start empty. Objects within one
    // shape class share a Structure, so all racers install against the
    // same StructureRareData.
    const list = [];
    for (let s = 0; s < SHAPES; ++s) {
        for (let copy = 0; copy < 4; ++copy) {
            const o = {};
            for (let j = 0; j <= s; ++j)
                o["r" + r + "p" + j] = j * 3 + 1;
            list.push(o);
        }
    }
    return list;
}

function sweep(r, list) {
    // for-in every object; each shape's first sweep is the racing install,
    // later sweeps are racing cached reads (flag-word load).
    for (let idx = 0; idx < list.length; ++idx) {
        const o = list[idx];
        const expected = (idx / 4 | 0) + 1; // shape s has s+1 properties
        let count = 0;
        let sum = 0;
        for (const k in o) {
            ++count;
            if (!k.startsWith("r" + r + "p"))
                throw new Error("round " + r + ": alien key " + k);
            sum += o[k];
        }
        if (count !== expected)
            throw new Error("round " + r + ": object " + idx + " enumerated " + count + " keys, expected " + expected);
        // sum of j*3+1 for j in [0, expected)
        const expectedSum = 3 * (expected - 1) * expected / 2 + expected;
        if (sum !== expectedSum)
            throw new Error("round " + r + ": object " + idx + " value sum " + sum + ", expected " + expectedSum);
    }
}

const racers = spawnN(N, which => {
    let seen = 0;
    for (;;) {
        let r;
        while ((r = Atomics.load(g, "round")) === seen)
            Atomics.wait(g, "round", seen, 100);
        if (r === -1)
            return "done";
        sweep(r, g.list);
        seen = r;
        Atomics.add(g, "done", 1);
    }
});

for (let r = 1; r <= ROUNDS; ++r) {
    g.list = buildList(r);
    Atomics.store(g, "done", 0);
    Atomics.store(g, "round", r);
    Atomics.notify(g, "round", Infinity);
    sweep(r, g.list); // main races the same installs
    waitUntil(() => Atomics.load(g, "done") === N);
}

Atomics.store(g, "round", -1);
Atomics.notify(g, "round", Infinity);
shouldBe(joinAll(racers).join(","), "done,done,done,done");
