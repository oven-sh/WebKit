//@ requireOptions("--useJSThreads=1")
// A delete from a cacheable dictionary makes a structure that is still a
// cacheable dictionary, and holds an unpinned copy of the property table, as
// upstream does. The threads delete path assumed that every cacheable
// dictionary's table is pinned, and a second delete aborted. That happened on
// one thread, with the flag on: an object with many properties, and two
// deletes.
//
// The second part runs the same shape on two threads: one deletes while the
// other adds, which pins the table in place.
load("../harness.js", "caller relative");

const KEYS = 200;

function makeDictionary() {
    const o = {};
    for (let i = 0; i < KEYS; ++i)
        o["k" + i] = i;
    return o;
}

{
    const o = makeDictionary();
    shouldBeTrue(delete o.k1, "first delete");
    shouldBeTrue(delete o.k2, "second delete");
    shouldBeTrue(delete o.k3, "third delete");
    shouldBeFalse("k2" in o, "k2 is gone");
    shouldBe(o.k4, 4, "k4 is still there");
    shouldBe(Object.keys(o).length, KEYS - 3, "key count");
}

{
    const OBJS = 60;
    const targets = [];
    for (let i = 0; i < OBJS; ++i) {
        const o = makeDictionary();
        delete o.k0; // The structure is now a cacheable dictionary with an unpinned table.
        targets.push(o);
    }

    const gate = { ready: 0, go: 0 };
    function startTogether(g) {
        Atomics.add(g, "ready", 1);
        while (Atomics.load(g, "go") === 0)
            Atomics.wait(g, "go", 0, 2);
    }
    const deleter = new Thread((objs, g) => {
        startTogether(g);
        for (const o of objs) {
            for (let i = 1; i < 20; ++i)
                delete o["k" + i];
        }
        return true;
    }, targets, gate);
    const adder = new Thread((objs, g) => {
        startTogether(g);
        for (const o of objs) {
            for (let i = 0; i < 20; ++i)
                o["added" + i] = i;
        }
        return true;
    }, targets, gate);

    waitUntil(() => Atomics.load(gate, "ready") === 2);
    Atomics.store(gate, "go", 1);
    Atomics.notify(gate, "go", Infinity);
    deleter.join();
    adder.join();

    for (let n = 0; n < OBJS; ++n) {
        const o = targets[n];
        for (let i = 1; i < 20; ++i)
            shouldBeFalse(("k" + i) in o, "object " + n + ": k" + i + " is deleted");
        for (let i = 20; i < KEYS; ++i)
            shouldBe(o["k" + i], i, "object " + n + ": k" + i + " is kept");
        for (let i = 0; i < 20; ++i)
            shouldBe(o["added" + i], i, "object " + n + ": added" + i + " is not lost");
    }
}
