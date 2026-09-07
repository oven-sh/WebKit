//@ requireOptions("--useJSThreads=1")
// One thread turns a shared object's property from a data property into an
// accessor and back, over and over, while other threads read it through every
// tier's property caches. A reader must always get either the data value or
// the getter's value: never the engine's internal getter/setter cell under the
// data property's structure, never a call of a plain value under the
// accessor's structure.
load("../harness.js", "caller relative");

const READERS = 4;
const FLIPS = 3000;
const READS = 60000; // per reader; bounded, so the test also ends under the cooperative GIL

function makeShared() {
    const o = { a: 0, x: 1, b: 2 };
    return o;
}

// Several objects, so that readers see the structures through polymorphic and
// monomorphic caches both.
const objects = [makeShared(), makeShared(), makeShared()];
const state = { go: 0 };

const readers = spawnN(READERS, (t) => {
    // A fresh function per thread so each gets its own inline caches too.
    const get = new Function("o", "return o.x;");
    const getByVal = new Function("o", "k", "return o[k];");
    Atomics.wait(state, "go", 0); // released by the notify below; GIL-on this parks with the GIL dropped
    let bad = 0;
    let reads = 0;
    let firstBad = null;
    for (let k = 0; k < READS; ++k) {
        const o = objects[k % objects.length];
        const v = (k & 1) ? get(o) : getByVal(o, "x");
        ++reads;
        if (v !== 1 && v !== 2) {
            ++bad;
            if (firstBad === null)
                firstBad = typeof v;
        }
    }
    return { bad, reads, firstBad };
});

const writer = new Thread(() => {
    Atomics.wait(state, "go", 0);
    for (let i = 0; i < FLIPS; ++i) {
        const o = objects[i % objects.length];
        Object.defineProperty(o, "x", { get() { return 2; }, configurable: true, enumerable: true });
        Object.defineProperty(o, "x", { value: 1, writable: true, configurable: true, enumerable: true });
    }
});

sleepMs(50); // let the threads reach their waits, so that they start together
Atomics.store(state, "go", 1);
Atomics.notify(state, "go");
writer.join();
const results = joinAll(readers);
for (let t = 0; t < READERS; ++t) {
    shouldBe(results[t].bad, 0, "bad reads on thread " + t + " (first bad value had typeof " + results[t].firstBad + ", of " + results[t].reads + " reads)");
    shouldBeTrue(results[t].reads > 0, "thread " + t + " read something");
}
for (const o of objects) {
    shouldBe(o.x, 1, "final value");
    shouldBe(Object.getOwnPropertyDescriptor(o, "x").value, 1, "final descriptor");
    shouldBe(Object.keys(o).join(), "a,x,b", "property order kept");
}
