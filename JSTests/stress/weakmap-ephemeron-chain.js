//@ runDefault("--useDollarVM=1", "--ephemeronFixpointIterations=1")
//@ runDefault("--useDollarVM=1", "--ephemeronFixpointIterations=1", "--verifyGC=1")
//@ runDefault("--useDollarVM=1", "--ephemeronFixpointIterations=1", "--useConcurrentGC=1", "--collectContinuously=1")
//@ runDefault("--useDollarVM=1", "--ephemeronFixpointIterations=1", "--numberOfGCMarkers=1")
//@ runDefault("--useDollarVM=1", "--ephemeronFixpointIterations=1", "--numberOfGCMarkers=1", "--gcIncrementScale=1", "--useConcurrentGC=1", "--collectContinuously=1")
//@ runDefault("--useDollarVM=1")
// The collector resolves a chain of WeakMap entries through a key-to-values table once the
// chain has cost it Options::ephemeronFixpointIterations() marking rounds. Every shape must
// keep all of its values alive in a bounded number of marking rounds, and the table must not
// keep the values of dead keys alive. weakmap-ephemeron-chain-long.js checks the time it takes.

load("./resources/weakmap-ephemeron-chains.js", "caller relative");

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(`${message}: expected ${expected}, got ${actual}`);
}

const count = 2000;
// Without the table a chain of this length takes about count / 2 rounds. With it, a round
// per threshold step, a few for the rest, and with several markers a hand-off or two per
// map that marking reaches late.
const roundsBound = 100;

function checkReachable(name, fixture) {
    let visited = 0;
    for (let node = fixture.head; node; node = fixture.walk(node))
        visited++;
    shouldBe(visited, count + 1, `${name}: nodes reachable after GC`);
}

function checkRounds(name) {
    const rounds = $vm.fixpointRoundsOfLastGC();
    if (rounds > roundsBound)
        throw new Error(`${name}: the last collection ran ${rounds} marking rounds`);
}

for (const [name, build] of Object.entries(ephemeronChainShapes(count))) {
    // A chain that has never been through a full collection: every key is new.
    let fixture = build();
    globalThis.fixture = fixture;
    edenGC();
    checkRounds(`${name} (eden)`);
    edenGC();
    checkReachable(`${name} (eden)`, fixture);
    fullGC();
    checkRounds(`${name} (full)`);
    fullGC();
    checkRounds(`${name} (second full)`);
    edenGC();
    checkReachable(`${name} (full)`, fixture);
    fixture = null;
    globalThis.fixture = null;
}

// A map that marking reaches only through the last link of a chain, while the program deletes
// some of its entries, replaces others and adds a few between collections.
{
    const chain = new WeakMap();
    const head = { id: 0 };
    let tail = head;
    for (let id = 1; id <= count; id++) {
        const next = { id };
        chain.set(tail, next);
        tail = next;
    }
    const late = new WeakMap();
    const keys = [];
    for (let i = 0; i < 200; i++) {
        const key = { i };
        keys.push(key);
        late.set(key, { i });
    }
    chain.set(tail, { late, keys });
    tail = null;
    globalThis.fixture = { head, chain };
    const deleted = new Set();
    for (let round = 0; round < 3; round++) {
        fullGC();
        checkRounds(`mutated late map (round ${round})`);
        let node = head;
        while (chain.get(node)?.late === undefined)
            node = chain.get(node);
        const { late: map, keys: held } = chain.get(node);
        for (let i = 0; i < held.length; i++) {
            shouldBe(map.has(held[i]), !deleted.has(held[i]), `mutated late map: entry ${i} after round ${round}`);
            if (i % 7 === round)
                deleted.add(held[i]), map.delete(held[i]);
            else if (i % 7 === round + 1)
                map.set(held[i], { replaced: i });
        }
        for (let i = 0; i < 20; i++) {
            const key = { added: round, i };
            held.push(key);
            map.set(key, { added: i });
        }
    }
    globalThis.fixture = null;
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
    edenGC();
    edenGC();
    checkReachable("mixed generations (eden)", { head, walk: node => map.get(node) });
    fullGC();
    checkReachable("mixed generations (full)", { head, walk: node => map.get(node) });
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
