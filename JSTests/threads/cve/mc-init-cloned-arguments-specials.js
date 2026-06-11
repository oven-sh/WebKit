//@ requireOptions("--useJSThreads=1")
// MC-INIT surface 9 (docs/threads/cve/map-MC-INIT.md):
// ClonedArguments::materializeSpecials publication order.
//
// Landing gate for UNGIL-HANDOUT AUD1.N3 / RESOLVED-4 (BINDING, unlanded
// at authoring time): the m_callee flag word (doubles as the
// not-yet-materialized flag) must be release-stored AFTER the OM puts of
// callee/@@iterator; foreign slow-path readers acquire. Today's in-tree
// ClonedArguments.cpp:283-299 does putDirect(callee),
// putDirect(@@iterator), then a PLAIN m_callee.clear() — a foreign
// reader observing the cleared flag before the puts misses the specials
// entirely (lost property; the "being-initialized state leaking
// defaults" MC-INIT sub-shape).
//
// Owner thread mints sloppy-mode ClonedArguments (via f.apply spread of
// a sloppy outer capturing strict semantics is unreliable across
// engines, so we use a STRICT function: strict `arguments` is
// ClonedArguments in JSC), publishes each through a shared slot, then
// triggers materializeSpecials (Object.keys). Reader threads
// concurrently probe the SAME object. Invariant in EVERY interleaving:
//   - args.length is exactly 3 (own data property from creation);
//   - getOwnPropertyDescriptor(args, "callee") is never undefined
//     (strict => accessor; reader-side access may itself materialize,
//     so absence = lost property = failure);
//   - args[Symbol.iterator] is always callable;
//   - indexed args are the minted values.
//
// EXECUTE POST-UNGIL ONLY. Amplifier-ready (fresh object per iteration
// keeps the race on the materialization edge every time).
load("../harness.js", "caller relative");

const N = 4; // reader threads
const ITERATIONS = 20000;

function mint(a, b, c) {
    "use strict";
    return arguments; // ClonedArguments
}

const shared = { slot: null, stop: 0, started: 0, go: 0 };

const readers = spawnN(N, (index) => {
    Atomics.add(shared, "started", 1);
    while (Atomics.load(shared, "go") === 0)
        Atomics.wait(shared, "go", 0, 100);

    let failures = 0;
    let observed = 0;
    while (!Atomics.load(shared, "stop")) {
        const args = shared.slot;
        if (args === null)
            continue;
        ++observed;
        if (args.length !== 3)
            ++failures;
        const calleeDesc = Object.getOwnPropertyDescriptor(args, "callee");
        if (calleeDesc === undefined)
            ++failures; // lost property: flag seen cleared before the put landed
        const iter = args[Symbol.iterator];
        if (typeof iter !== "function")
            ++failures; // lost @@iterator
        if (args[1] !== "two")
            ++failures; // indexed contents must be creation-time values
    }
    return failures;
});

waitUntil(() => Atomics.load(shared, "started") === N);
Atomics.store(shared, "go", 1);
Atomics.notify(shared, "go");

// Owner: mint, publish, then trigger materializeSpecials on the published
// object while readers race it. Object.keys / gOPN both route through
// materializeSpecialsIfNecessary.
for (let i = 0; i < ITERATIONS; ++i) {
    const args = mint(1, "two", { three: 3 });
    shared.slot = args; // publish FIRST: readers race the materialization
    Object.keys(args); // owner-side materializeSpecials
    if ((i & 1023) === 0)
        sleepMs(1); // let readers catch up under the cooperative scheduler
}
Atomics.store(shared, "stop", 1);

for (const failures of joinAll(readers))
    shouldBe(failures, 0, "lost callee/@@iterator on racing materializeSpecials (MC-INIT AUD1.N3)");
