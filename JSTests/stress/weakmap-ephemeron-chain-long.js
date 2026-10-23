//@ skip if $buildType == "debug"
//@ runDefault
//@ runDefault("--ephemeronFixpointIterations=1")
//@ runDefault("--useConcurrentGC=1", "--collectContinuously=1")
// Marking a chain of WeakMap entries must not take a marking round per link. Before the
// key-to-values table, two full collections of the first shape below took about 18 seconds,
// because the "O" constraint rescanned the whole map in each of the ~N/2 rounds. With the
// table they take tens of milliseconds. The bound is on wall-clock time, so a debug build is
// skipped. --verifyGC=1 is not in the list: the verifier marks with its own visitor and still
// takes a round per link.

load("./resources/weakmap-ephemeron-chains.js", "caller relative");

const count = 40000;

function check(name, fixture, collect) {
    globalThis.fixture = fixture;
    const start = Date.now();
    collect();
    collect();
    const elapsed = Date.now() - start;
    if (elapsed > 5000)
        throw new Error(`${name}: two collections took ${elapsed} ms`);
    let visited = 0;
    for (let node = fixture.head; node; node = fixture.walk(node))
        visited++;
    if (visited !== count + 1)
        throw new Error(`${name}: expected ${count + 1} nodes reachable after GC, got ${visited}`);
    globalThis.fixture = null;
}

for (const [name, build] of Object.entries(ephemeronChainShapes(count))) {
    check(`${name} full`, build(), fullGC);
    // A chain that has never been through a full collection: every key is new.
    check(`${name} eden`, build(), edenGC);
}

// Half of the chain survived a full collection, the other half is new.
{
    const map = new WeakMap();
    const head = { id: 0 };
    let tail = head;
    for (let id = 1; id <= count / 2; id++) {
        const next = { id };
        map.set(tail, next);
        tail = next;
    }
    globalThis.fixture = { head, map };
    fullGC();
    for (let id = count / 2 + 1; id <= count; id++) {
        const next = { id };
        map.set(tail, next);
        tail = next;
    }
    tail = null;
    check("mixed generations eden", { head, roots: [map], walk: node => map.get(node) }, edenGC);
}
