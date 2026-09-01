//@ requireOptions("--useJSThreads=1")
// Shared-object semantics: dictionary-mode objects across threads. In the
// design, dictionary-mode reads/writes take the per-object lock (regime 3).
// We cannot assert the internal mode from script without $vm, so each section
// first pushes the object into dictionary mode the way JSC actually gets
// there (deletes, huge transition counts, uncacheable redefinitions) and then
// exercises cross-thread reads/writes/adds/deletes on it.
load("../resources/assert.js", "caller relative");

// Helper: deleting a non-last property is the classic way into an
// uncacheable dictionary structure in JSC.
function makeDictionaryByDelete() {
    const o = { d0: 0, d1: 1, d2: 2, d3: 3 };
    delete o.d1;
    return o;
}

// Helper: a long transition chain flattens into a cacheable dictionary.
function makeDictionaryByManyTransitions() {
    const o = {};
    for (let i = 0; i < 1100; ++i)
        o["t" + i] = i;
    return o;
}

// --- Cross-thread read/write on a delete-induced dictionary.
{
    const obj = makeDictionaryByDelete();
    const seen = new Thread(o => {
        const r = o.d0 + "," + o.d2 + "," + o.d3 + "," + ("d1" in o);
        o.d2 = 22;
        return r;
    }, obj).join();
    shouldBe(seen, "0,2,3,false");
    shouldBe(obj.d2, 22);
}

// --- Cross-thread add and delete on a dictionary object.
{
    const obj = makeDictionaryByDelete();
    new Thread(o => {
        o.addedInDict = "yes";
        delete o.d3;
    }, obj).join();
    shouldBe(obj.addedInDict, "yes");
    shouldBeFalse("d3" in obj);
    shouldBe(Object.keys(obj).join(","), "d0,d2,addedInDict");
}

// --- A foreign thread drives an object into dictionary mode (via delete)
// while main holds a reference; main's subsequent accesses stay correct.
{
    const obj = { a: 1, b: 2, c: 3 };
    new Thread(o => { delete o.b; o.d = 4; }, obj).join();
    shouldBe(obj.a, 1);
    shouldBe(obj.b, undefined);
    shouldBe(obj.c, 3);
    shouldBe(obj.d, 4);
    shouldBe(Object.keys(obj).join(","), "a,c,d");
}

// --- Huge-transition-chain dictionary: foreign reads, overwrites, and a
// full enumeration agree with the main thread's view.
{
    const big = makeDictionaryByManyTransitions();
    const result = new Thread(o => {
        let sum = 0;
        for (let i = 0; i < 1100; ++i)
            sum += o["t" + i];
        o.t555 = -555;
        return sum + ":" + Object.keys(o).length;
    }, big).join();
    shouldBe(result, (1099 * 1100 / 2) + ":1100");
    shouldBe(big.t555, -555);
    shouldBe(big.t554, 554);
    shouldBe(big.t556, 556);
}

// --- Many threads mutating one dictionary object under a lock: adds,
// overwrites, and deletes interleave with no corruption.
{
    const dict = makeDictionaryByDelete();
    const lock = new Lock();
    const threadCount = 6, rounds = 40;
    joinAll(spawnN(threadCount, i => {
        for (let r = 0; r < rounds; ++r) {
            lock.hold(() => {
                dict["mine" + i] = r;        // add or overwrite
                dict.shared = (dict.shared | 0) + 1;
                if (r % 2)
                    delete dict["temp" + i];
                else
                    dict["temp" + i] = i;
            });
        }
    }));
    shouldBe(dict.shared, threadCount * rounds);
    for (let i = 0; i < threadCount; ++i) {
        shouldBe(dict["mine" + i], rounds - 1);
        // rounds is even, so the last action on temp_i (r = rounds-1, odd) was delete.
        shouldBeFalse(("temp" + i) in dict);
    }
    // Original survivors unscathed.
    shouldBe(dict.d0, 0);
    shouldBe(dict.d2, 2);
}

// --- Property enumeration snapshot of a dictionary taken inside a thread
// matches the joined main-thread view, including symbol keys.
{
    const sym = Symbol("dict-sym");
    const obj = makeDictionaryByDelete();
    obj[sym] = "symval";
    const snap = new Thread(o => Object.keys(o).join(",") + "|" + Object.getOwnPropertySymbols(o).length, obj).join();
    shouldBe(snap, "d0,d2,d3|1");
    shouldBe(obj[sym], "symval");
}

// --- Accessors on a dictionary object defined and used cross-thread.
{
    const obj = makeDictionaryByDelete();
    new Thread(o => {
        let store = 0;
        Object.defineProperty(o, "dictAcc", {
            get() { return store; },
            set(v) { store = v * 2; },
            configurable: true,
        });
    }, obj).join();
    obj.dictAcc = 21;
    shouldBe(obj.dictAcc, 42);
    shouldBe(new Thread(o => { o.dictAcc = 5; return o.dictAcc; }, obj).join(), 10);
}

// --- A dictionary-mode prototype: chain lookups from foreign threads.
{
    const proto = makeDictionaryByDelete();
    proto.protoOnly = "via-chain";
    const obj = Object.create(proto);
    shouldBe(new Thread(o => o.protoOnly + ":" + o.d0, obj).join(), "via-chain:0");
    new Thread(p => { p.protoOnly = "updated"; }, proto).join();
    shouldBe(obj.protoOnly, "updated");
}

// --- Re-shaping back out: after heavy delete/add churn from several threads
// (serialized by join), the object still behaves like a plain object.
{
    const obj = {};
    for (let round = 0; round < 4; ++round) {
        new Thread((o, r) => {
            for (let i = 0; i < 30; ++i)
                o["churn" + i] = r * 100 + i;
            for (let i = 0; i < 30; i += 2)
                delete o["churn" + i];
        }, obj, round).join();
    }
    // Odd-indexed keys from the last round survive.
    for (let i = 1; i < 30; i += 2)
        shouldBe(obj["churn" + i], 300 + i);
    for (let i = 0; i < 30; i += 2)
        shouldBeFalse(("churn" + i) in obj);
    shouldBe(Object.keys(obj).length, 15);
    obj.afterChurn = "ok";
    shouldBe(new Thread(o => o.afterChurn, obj).join(), "ok");
}
