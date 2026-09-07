//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// N1-I (SPEC-objectmodel rev 16) stamps the allocating thread's TID into a
// butterfly-less object's word. A FOREIGN thread's first out-of-line add on
// such an object (here a cacheable dictionary, so the add takes the locked
// path and installs fresh out-of-line storage) reached the tagged-word store
// with a foreign-owned payload-free word and release-asserted ("must be empty
// or owner-tagged") - the pre-r16 word was 0 there. It is the N3 first-install
// shape: the installer becomes the owner. Found by the mirror harness
// (stress/megamorphic-instance-dictionary-miss.js, 16 of 30 aborted).
load("../harness.js", "caller relative");

function makeObjects(n) {
    const out = [];
    for (let i = 0; i < n; ++i) {
        // Object literal with its inline capacity exactly used, then made a
        // dictionary: the next add needs out-of-line storage.
        const o = { a: i, b: i + 1, c: i + 2, d: i + 3, e: i + 4, f: i + 5 };
        $vm.toCacheableDictionary(o);
        out.push(o);
    }
    return out;
}

for (let round = 0; round < 20; ++round) {
    const objs = makeObjects(64);                       // owned by main, butterfly-less
    const t = new Thread(() => {
        for (const o of objs) { o.x1 = 1; o.x2 = 2; o.x3 = 3; }   // foreign first out-of-line adds
        return objs.length;
    });
    // Odd rounds: the foreign thread alone does the first out-of-line add (the
    // aborting shape); even rounds: the owner races it with its own adds.
    if (round & 1) {
        if (t.join() !== 64) throw new Error("thread result");
        for (const o of objs) { o.y1 = 10; o.y2 = 20; }
    } else {
        for (const o of objs) { o.y1 = 10; o.y2 = 20; }
        if (t.join() !== 64) throw new Error("thread result");
    }
    for (let i = 0; i < objs.length; ++i) {
        const o = objs[i];
        if (o.a !== i || o.f !== i + 5 || o.x1 !== 1 || o.x3 !== 3 || o.y1 !== 10 || o.y2 !== 20)
            throw new Error("lost property on object " + i + ": " + JSON.stringify(o));
    }
}
print("PASS");
