//@ requireOptions("--useJSThreads=1")
// SPEC-objectmodel N2-LF vs N3: the owner adds named properties to butterfly-
// less objects through the cached transition (which claims the StructureID
// lane with a compare-and-swap) while another thread does the FIRST indexed
// store (o[0] = v: a butterfly install that nukes and republishes the same
// header) on the very same objects. Every object must end with all named
// properties and the indexed element; no add may be lost and nothing may
// crash on a torn {structure, butterfly} pair.
load("../harness.js", "caller relative");

const OBJECTS = 4000;
const ROUNDS = 3;

function addNamed(o, v) { o.a = v; o.b = v + 1; o.c = v + 2; }
noInline(addNamed);

// Warm the transition cache on the owner thread first.
for (let i = 0; i < 20000; ++i)
    addNamed({}, i);

for (let round = 0; round < ROUNDS; ++round) {
    const objects = [];
    for (let i = 0; i < OBJECTS; ++i)
        objects.push({});
    const gate = { go: 0 };
    const indexer = new Thread((objs, g) => {
        Atomics.wait(g, "go", 0);
        for (let i = 0; i < objs.length; ++i)
            objs[i][0] = i;
        return objs.length;
    }, objects, gate);
    sleepMs(20);
    Atomics.store(gate, "go", 1);
    Atomics.notify(gate, "go");
    // Race the indexer from the other end so the two meet in the middle.
    for (let i = OBJECTS - 1; i >= 0; --i)
        addNamed(objects[i], i);
    shouldBe(indexer.join(), OBJECTS, "indexed stores in round " + round);
    for (let i = 0; i < OBJECTS; ++i) {
        const o = objects[i];
        if (o.a !== i || o.b !== i + 1 || o.c !== i + 2 || o[0] !== i)
            throw new Error("round " + round + " object " + i + " is " + JSON.stringify(o) + " [0]=" + o[0]);
    }
}
