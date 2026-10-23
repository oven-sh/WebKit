//@ runDefault("--ephemeronFixpointIterations=1")
//@ runDefault("--ephemeronFixpointIterations=1", "--verifyGC=1")
//@ runDefault("--ephemeronFixpointIterations=1", "--useConcurrentGC=1", "--collectContinuously=1")
//@ runDefault("--ephemeronFixpointIterations=1", "--numberOfGCMarkers=1")
//@ runDefault
// The collector resolves a chain of WeakMap entries through a key-to-values table once the
// chain has cost it Options::ephemeronFixpointIterations() marking rounds. Every shape must
// keep all of its values alive, and the table must not keep the values of dead keys alive.
// weakmap-ephemeron-chain-long.js checks the time it takes.

load("./resources/weakmap-ephemeron-chains.js", "caller relative");

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected}, got ${actual}`);
}

const count = 2000;

for (const [name, build] of Object.entries(ephemeronChainShapes(count))) {
    const fixture = build();
    globalThis.fixture = fixture;
    fullGC();
    fullGC();
    edenGC();
    let visited = 0;
    for (let node = fixture.head; node; node = fixture.walk(node))
        visited++;
    shouldBe(visited, count + 1, `${name}: nodes reachable after GC`);
    globalThis.fixture = null;
}

// Values whose keys died must be collected, also when the collector resolves the map through
// its key-to-values table. The live chain makes it build that table. The dead chain is in
// the same map, so its entries are in the table too.
const deadCount = 200;
let collected = 0;
const registry = new FinalizationRegistry(() => { collected++; });

function addDeadChain(map) {
    let tail = { id: 0 };
    for (let id = 1; id <= deadCount; id++) {
        const next = { id };
        registry.register(next);
        map.set(tail, next);
        tail = next;
    }
}
noInline(addDeadChain);

function addLiveChain(map) {
    const head = { id: 0 };
    let tail = head;
    for (let id = 1; id <= count; id++) {
        const next = { id };
        map.set(tail, next);
        tail = next;
    }
    return head;
}
noInline(addLiveChain);

const mixedMap = new WeakMap();
addDeadChain(mixedMap);
const liveHead = addLiveChain(mixedMap);
globalThis.fixture = { map: mixedMap, head: liveHead };
setTimeout(() => {
    fullGC();
    fullGC();
    setTimeout(() => {
        shouldBe(collected, deadCount, "values of dead keys are collected");
        let visited = 0;
        for (let node = liveHead; node; node = mixedMap.get(node))
            visited++;
        shouldBe(visited, count + 1, "live chain stays reachable");
        globalThis.fixture = null;
    }, 0);
}, 0);
