//@ runDefault
//@ runDefault("--ephemeronFixpointIterations=1")
//@ runDefault("--useConcurrentGC=1", "--collectContinuously=1")
// Marking a chain of WeakMap entries must not take a marking round per link. Before the
// key-to-values table, a full collection of the first shape below took about 18 seconds,
// because the "O" constraint rescanned the whole map in each of the ~N/2 rounds. With the
// table it takes tens of milliseconds. --verifyGC=1 is not in the list: the verifier marks
// with its own visitor and still takes a round per link.

load("./resources/weakmap-ephemeron-chains.js", "caller relative");

const count = 40000;

for (const [name, build] of Object.entries(ephemeronChainShapes(count))) {
    const fixture = build();
    globalThis.fixture = fixture;
    const start = Date.now();
    fullGC();
    fullGC();
    const elapsed = Date.now() - start;
    if (elapsed > 5000)
        throw new Error(`${name}: two full collections took ${elapsed} ms`);
    let visited = 0;
    for (let node = fixture.head; node; node = fixture.walk(node))
        visited++;
    if (visited !== count + 1)
        throw new Error(`${name}: expected ${count + 1} nodes reachable after GC, got ${visited}`);
    globalThis.fixture = null;
}
