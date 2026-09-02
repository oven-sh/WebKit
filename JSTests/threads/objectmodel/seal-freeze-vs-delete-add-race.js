//@ requireOptions("--useJSThreads=1")
// Object.seal, Object.freeze and Object.preventExtensions change only the
// structure. With the GIL off, they read the structure, planned the new one,
// and stored it with no check. A delete or an add on another thread could
// publish its own structure in between, and the store then undid it:
//
// - seal or freeze after a delete brought the property back with no value
//   (hasOwnProperty was true, and the value was undefined);
// - preventExtensions after an add removed the property, although the add had
//   returned without a TypeError.
//
// Each round races one thread against another over many fresh objects, so a
// lost update shows up in most runs.
load("../harness.js", "caller relative");

const ROUNDS = 12;
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

function raceWithDelete(kind, round) {
    const targets = [];
    for (let i = 0; i < OBJS; ++i)
        targets.push({ keep: i, doomed: "d" + i });

    const gate = { ready: 0, go: 0 };
    const changer = new Thread((objs, g, k) => {
        startTogether(g);
        for (let i = 0; i < objs.length; ++i) {
            if (k === "seal")
                Object.seal(objs[i]);
            else
                Object.freeze(objs[i]);
        }
        return true;
    }, targets, gate, kind);
    const deleter = new Thread((objs, g) => {
        startTogether(g);
        for (let i = 0; i < objs.length; ++i)
            delete objs[i].doomed; // Sloppy mode: false when the seal came first.
        return true;
    }, targets, gate);

    release(gate, 2);
    changer.join();
    deleter.join();

    for (let i = 0; i < OBJS; ++i) {
        const o = targets[i];
        const where = kind + " round " + round + ", object " + i;
        shouldBe(o.keep, i, where + ": keep");
        if (Object.prototype.hasOwnProperty.call(o, "doomed"))
            shouldBe(o.doomed, "d" + i, where + ": a doomed property that is still there keeps its value");
    }
}

function raceWithAdd(round) {
    const targets = [];
    for (let i = 0; i < OBJS; ++i)
        targets.push({ keep: i });

    const gate = { ready: 0, go: 0 };
    const preventer = new Thread((objs, g) => {
        startTogether(g);
        for (let i = 0; i < objs.length; ++i)
            Object.preventExtensions(objs[i]);
        return true;
    }, targets, gate);
    const adder = new Thread((objs, g) => {
        "use strict";
        startTogether(g);
        const added = [];
        for (let i = 0; i < objs.length; ++i) {
            try {
                objs[i].added = i;
                added.push(i);
            } catch (e) {
                if (!(e instanceof TypeError))
                    throw e;
            }
        }
        return added;
    }, targets, gate);

    release(gate, 2);
    preventer.join();
    const added = adder.join();

    const wasAdded = new Array(OBJS).fill(false);
    for (const i of added)
        wasAdded[i] = true;
    for (let i = 0; i < OBJS; ++i) {
        const o = targets[i];
        const where = "preventExtensions round " + round + ", object " + i;
        shouldBeFalse(Object.isExtensible(o), where + ": not extensible");
        shouldBe(Object.prototype.hasOwnProperty.call(o, "added"), wasAdded[i], where + ": the property is there exactly when the add did not throw");
        if (wasAdded[i])
            shouldBe(o.added, i, where + ": value");
    }
}

for (let round = 0; round < ROUNDS; ++round) {
    raceWithDelete("seal", round);
    raceWithDelete("freeze", round);
    raceWithAdd(round);
}
