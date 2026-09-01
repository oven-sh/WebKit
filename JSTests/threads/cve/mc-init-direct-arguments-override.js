//@ requireOptions("--useJSThreads=1")
// MC-INIT surface 10 (docs/threads/cve/map-MC-INIT.md): DirectArguments
// lazy override storage (m_mappedArguments) publication.
//
// Landing gate for UNGIL-HANDOUT AUD1.N3 / RESOLVED-3 (BINDING, unlanded
// at authoring time): m_mappedArguments (+ modified-arguments descriptor
// bitmap) must be CAS-PUBLISHed — allocate + fill COMPLETE, release-CAS
// the pointer, losers discard, readers load-acquire; the tier-inlined
// null-check stays as an address-dependent load. Today's in-tree
// DirectArguments.cpp:133-145 allocates the override bitmap, fills it,
// then publishes with a PLAIN m_mappedArguments.set(); overrideArgument
// (:164) then flips bytes in it. A foreign reader (interpreter or DFG
// GetFromArguments with the baked offsetOfMappedArguments null-check)
// can pair the published bitmap pointer with unfilled contents.
//
// Owner thread mints sloppy-mode DirectArguments, publishes through a
// shared slot, then triggers the override path (delete args[0]; write
// args.length). Reader threads concurrently probe. Invariant in EVERY
// interleaving:
//   - args[1] is exactly its creation-time value (never overridden);
//   - args[0] is the creation-time value OR absent (post-delete);
//     anything else = garbage through a half-built override bitmap;
//   - args.length is the creation-time 3 OR the owner's override 99.
//
// EXECUTE POST-UNGIL ONLY. Amplifier-ready (fresh object per iteration;
// run TSAN no-JIT first, then default tiers so the DFG inlined
// mapped-arguments check is exercised).
load("../harness.js", "caller relative");

const N = 4; // reader threads
const ITERATIONS = 20000;

function mint(a, b, c) {
    return arguments; // sloppy + simple parameters => DirectArguments
}

const shared = { slot: null, stop: 0, started: 0, go: 0 };
const SENTINEL1 = "value-one";

const readers = spawnN(N, (index) => {
    Atomics.add(shared, "started", 1);
    while (Atomics.load(shared, "go") === 0)
        Atomics.wait(shared, "go", 0, 100);

    let failures = 0;
    while (!Atomics.load(shared, "stop")) {
        const args = shared.slot;
        if (args === null)
            continue;

        const v1 = args[1];
        if (v1 !== SENTINEL1)
            ++failures; // untouched mapped slot must never change

        const v0 = args[0];
        if (!(v0 === 7 || v0 === undefined))
            ++failures; // creation value or deleted — never garbage

        const len = args.length;
        if (!(len === 3 || len === 99))
            ++failures; // creation length or the override — never a default leak
    }
    return failures;
});

waitUntil(() => Atomics.load(shared, "started") === N);
Atomics.store(shared, "go", 1);
Atomics.notify(shared, "go");

for (let i = 0; i < ITERATIONS; ++i) {
    const args = mint(7, SENTINEL1, true);
    shared.slot = args; // publish BEFORE overriding: readers race the
    delete args[0];     // overrideArgument -> first m_mappedArguments alloc
    args.length = 99;   // overrideThings family
    if ((i & 1023) === 0)
        sleepMs(1);
}
Atomics.store(shared, "stop", 1);

for (const failures of joinAll(readers))
    shouldBe(failures, 0, "half-built override storage observed (MC-INIT AUD1.N3 / DirectArguments)");
