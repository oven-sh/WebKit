//@ requireOptions("--useJSThreads=1")
// MC-INIT surface 8 (docs/threads/cve/map-MC-INIT.md): JSRopeString
// resolution / atomization on shared ropes.
//
// Landing gate for SPEC-ungil §N.2 (BINDING, unlanded at authoring time):
// rope resolution must be lock-free with the resolver computing into a
// FRESH buffer and publishing by ONE release-CAS of the fiber0/flags
// word; losers discard and re-read; readers load-acquire. Today's
// in-tree resolveRopeWithFunction / swapToAtomString
// (runtime/JSString.h:637-684, :875-912) mutate fiber words and
// valueInternal() in place with plain stores — a reader racing a
// resolver (or two racing resolvers) can observe a half-published
// {flags, fiber0} pair.
//
// K fresh ropes are built per round WITHOUT reading them (concatenation
// only), published in a shared array, then N threads concurrently force
// resolution (===, length, charCodeAt sampling) and atomization
// (computed-property-key use). Detector: every observation equals the
// independently-built flat expectation — never empty, torn, truncated,
// or a wrong character; the atom key lookup always round-trips.
//
// EXECUTE POST-UNGIL ONLY. Amplifier-ready: per-round fresh ropes keep
// the race window on the first-resolution edge every iteration
// (Tools/threads/amplify.sh; TSAN no-JIT first, then default tiers).
load("../harness.js", "caller relative");

const N = 6;
const ROUNDS = 50;
const K = 16; // ropes per round

// Build a rope of `parts` without resolving it; also return the flat
// expectation built by an independent path (Array.join flattens eagerly
// without touching the rope).
function buildRope(seed) {
    const parts = [];
    for (let i = 0; i < 24; ++i)
        parts.push(String.fromCharCode(97 + ((seed + i) % 26)) + i + "-");
    let rope = "";
    for (const p of parts)
        rope = rope + p; // rope concatenation; never read
    return { rope, flat: parts.join("") };
}

const shared = { round: -1, ropes: null, flats: null, done: 0, failures: 0, go: 0, started: 0 };

const threads = spawnN(N, (index) => {
    Atomics.add(shared, "started", 1);
    while (Atomics.load(shared, "go") === 0)
        Atomics.wait(shared, "go", 0, 100);

    let lastRound = -1;
    let failures = 0;
    for (;;) {
        const round = Atomics.load(shared, "round");
        if (round === -2)
            break; // shutdown
        if (round === lastRound || round < 0) {
            Atomics.wait(shared, "round", lastRound, 5); // bounded
            continue;
        }
        lastRound = round;
        const ropes = shared.ropes;
        const flats = shared.flats;
        for (let k = 0; k < ropes.length; ++k) {
            // Stagger start so different threads hit different ropes first.
            const j = (k + index) % ropes.length;
            const r = ropes[j];
            const f = flats[j];
            // Force resolution three independent ways.
            if (r.length !== f.length)
                ++failures; // torn/short resolution observed
            if (r !== f)
                ++failures;
            const mid = r.charCodeAt(f.length >> 1);
            if (mid !== f.charCodeAt(f.length >> 1))
                ++failures;
            // Atomization path (resolveRopeToAtomString / shared table):
            // a computed property key must round-trip.
            const o = {};
            o[r] = j;
            if (o[f] !== j)
                ++failures; // atomized rope and flat string disagree
        }
        Atomics.add(shared, "done", 1);
    }
    return failures;
});

waitUntil(() => Atomics.load(shared, "started") === N);
Atomics.store(shared, "go", 1);
Atomics.notify(shared, "go");

for (let round = 0; round < ROUNDS; ++round) {
    const ropes = [];
    const flats = [];
    for (let k = 0; k < K; ++k) {
        const { rope, flat } = buildRope(round * K + k);
        ropes.push(rope);
        flats.push(flat);
    }
    shared.ropes = ropes; // plain publish of the array, then the round bump
    shared.flats = flats; // is the cross-thread "new work" edge
    Atomics.store(shared, "done", 0);
    Atomics.store(shared, "round", round);
    Atomics.notify(shared, "round");
    waitUntil(() => Atomics.load(shared, "done") === N);
}
Atomics.store(shared, "round", -2);
Atomics.notify(shared, "round");

for (const failures of joinAll(threads))
    shouldBe(failures, 0, "torn/partial rope resolution observed (MC-INIT §N.2)");
