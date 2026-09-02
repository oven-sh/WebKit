//@ requireOptions("--useJSThreads=1")
// A RegExp split whose result comes from the split cache still updates the
// legacy RegExp statics (RegExp.lastMatch and friends). On a spawned thread
// those statics are the thread's own, so the update must land there and
// describe the split that was just done.
load("../harness.js", "caller relative");

const THREADS = 3;

function atomize(s) {
    // Using a string as a property key makes its StringImpl an atom, and the
    // split cache only holds atom inputs.
    const o = {};
    o[s] = 1;
    return Object.keys(o)[0];
}

const inputs = [];
for (let t = 0; t < THREADS; ++t)
    inputs.push(atomize("a" + t + ",b" + t + ",c" + t));

// Fills the cache on the main thread before any thread starts.
for (const input of inputs)
    input.split(/(,)/);

function work(t) {
    const input = inputs[t];
    const re = /(,)/;
    for (let i = 0; i < 50; ++i) {
        // Move this thread's statics away from the split's values.
        /(q)z/.exec("xq" + t + "z");
        const parts = input.split(re);
        if (parts.length !== 5 || parts[4] !== "c" + t)
            return "thread " + t + " split: " + JSON.stringify(parts);
        if (RegExp.lastMatch !== "," || RegExp.leftContext !== "a" + t + ",b" + t || RegExp.$1 !== ",")
            return "thread " + t + " statics: " + JSON.stringify([RegExp.lastMatch, RegExp.leftContext, RegExp.$1]);
    }
    return null;
}

for (const r of joinAll(spawnN(THREADS, work)))
    shouldBe(r, null);
shouldBe(work(0), null);
