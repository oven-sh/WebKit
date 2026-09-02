//@ requireOptions("--useJSThreads=1")
// Several operations change only an object's structure: a prototype change,
// becoming a prototype, the first indexed accessor, a private brand, and
// Object.assign into an empty object. With the GIL off, each one read the
// structure, planned the new one, and stored it with no check. A delete or an
// add on another thread could publish its own structure in between, and the
// store then undid it. A deleted property came back with no value, or an added
// property was lost.
//
// Each case races one such operation against a delete or an add, over many
// fresh objects.
load("../harness.js", "caller relative");

const ROUNDS = 6;
const OBJS = 200;

function startTogether(gate) {
    Atomics.add(gate, "ready", 1);
    while (Atomics.load(gate, "go") === 0)
        Atomics.wait(gate, "go", 0, 2);
}

function release(gate, threads) {
    waitUntil(() => Atomics.load(gate, "ready") === threads);
    Atomics.store(gate, "go", 1);
    Atomics.notify(gate, "go", Infinity);
}

class Base {
    constructor(target) { return target; }
}

class Branded extends Base {
    #brandedMethod() { return 1; }
    constructor(target) { super(target); }
    static has(o) { return #brandedMethod in o; }
}

const changes = {
    setPrototype(o) { Object.setPrototypeOf(o, { fromProto: 1 }); },
    becomePrototype(o) { Object.setPrototypeOf({}, o); },
    indexedAccessor(o) { Object.defineProperty(o, "0", { get() { return 7; }, configurable: true }); },
    privateBrand(o) { new Branded(o); },
};

function raceWithDelete(name, round) {
    const targets = [];
    for (let i = 0; i < OBJS; ++i)
        targets.push({ keep: i, doomed: "d" + i });

    const gate = { ready: 0, go: 0 };
    const changer = new Thread((objs, g, n) => {
        startTogether(g);
        for (let i = 0; i < objs.length; ++i)
            changes[n](objs[i]);
        return true;
    }, targets, gate, name);
    const deleter = new Thread((objs, g) => {
        startTogether(g);
        for (let i = 0; i < objs.length; ++i)
            delete objs[i].doomed;
        return true;
    }, targets, gate);

    release(gate, 2);
    changer.join();
    deleter.join();

    for (let i = 0; i < OBJS; ++i) {
        const o = targets[i];
        const where = name + " round " + round + ", object " + i;
        shouldBe(o.keep, i, where + ": keep");
        shouldBeFalse(Object.prototype.hasOwnProperty.call(o, "doomed"), where + ": the delete is not undone");
        if (name === "setPrototype")
            shouldBe(o.fromProto, 1, where + ": the new prototype");
        if (name === "indexedAccessor")
            shouldBe(o[0], 7, where + ": the accessor");
        if (name === "privateBrand")
            shouldBeTrue(Branded.has(o), where + ": the brand");
    }
}

function assignWithAdd(round) {
    const targets = [];
    for (let i = 0; i < OBJS; ++i)
        targets.push({});
    const source = { a: 1, b: 2 };

    const gate = { ready: 0, go: 0 };
    const assigner = new Thread((objs, g, src) => {
        startTogether(g);
        for (let i = 0; i < objs.length; ++i)
            Object.assign(objs[i], src);
        return true;
    }, targets, gate, source);
    const adder = new Thread((objs, g) => {
        startTogether(g);
        for (let i = 0; i < objs.length; ++i)
            objs[i].added = i;
        return true;
    }, targets, gate);

    release(gate, 2);
    assigner.join();
    adder.join();

    for (let i = 0; i < OBJS; ++i) {
        const o = targets[i];
        const where = "assign round " + round + ", object " + i;
        shouldBe(o.a, 1, where + ": a");
        shouldBe(o.b, 2, where + ": b");
        shouldBe(o.added, i, where + ": the add is not lost");
    }
}

for (let round = 0; round < ROUNDS; ++round) {
    for (const name of Object.keys(changes))
        raceWithDelete(name, round);
    assignWithAdd(round);
}
