//@ requireOptions("--useJSThreads=1")
// Reading the `get` or `set` of a built-in accessor property that the engine
// implements natively (RegExp.input, a typed array's `length`, ...) makes a
// function object for it on first use, cached per global object in a weak set
// keyed by the property. Several threads reifying such accessors at the same
// time race on that set: each must get a working function, the same one as
// every other thread for the same accessor while it is alive, and the set must
// survive its entries dying in a collection and being made again.
load("../harness.js", "caller relative");

const THREADS = 4;
const ROUNDS = 30;

const holders = [
    RegExp, RegExp.prototype, Symbol.prototype, Function.prototype, Error.prototype,
    ArrayBuffer.prototype, DataView.prototype, Object.getPrototypeOf(Int8Array.prototype),
    Int8Array.prototype, Float64Array.prototype, Map.prototype, Set.prototype,
    WeakMap.prototype, Promise.prototype, String.prototype, Array.prototype,
    globalThis, Intl.Locale ? Intl.Locale.prototype : {}, Date.prototype,
];
if (typeof SharedArrayBuffer === "function")
    holders.push(SharedArrayBuffer.prototype);

function touchAccessors() {
    let count = 0;
    for (const holder of holders) {
        for (const name of Object.getOwnPropertyNames(holder)) {
            const d = Object.getOwnPropertyDescriptor(holder, name);
            if (!d || !("get" in d))
                continue;
            if (typeof d.get === "function") {
                // The function must be usable: it has a name and a source text.
                if (typeof d.get.name !== "string" || String(d.get).indexOf("function") < 0)
                    throw new Error("bad getter function for " + name);
                ++count;
            }
            if (typeof d.set === "function") {
                if (String(d.set).indexOf("function") < 0)
                    throw new Error("bad setter function for " + name);
                ++count;
            }
        }
    }
    return count;
}

const expected = touchAccessors();
shouldBeTrue(expected > 20, "the test touches some accessors (" + expected + ")");

const gate = { go: 0 };
const threads = spawnN(THREADS, (t) => {
    Atomics.wait(gate, "go", 0);
    let total = 0;
    for (let i = 0; i < ROUNDS; ++i) {
        total += touchAccessors();
        if ((i + t) % 7 === 0)
            gc(); // let cached functions die, so that they are made again
    }
    return total;
});
sleepMs(50);
Atomics.store(gate, "go", 1);
Atomics.notify(gate, "go");
const results = joinAll(threads);
for (let t = 0; t < THREADS; ++t)
    shouldBe(results[t], expected * ROUNDS, "accessor functions seen by thread " + t);

// Identity: while alive, one accessor reifies to one function for every thread.
const keep = Object.getOwnPropertyDescriptor(RegExp, "input").get;
const seen = joinAll(spawnN(THREADS, () => Object.getOwnPropertyDescriptor(RegExp, "input").get === keep));
for (const same of seen)
    shouldBeTrue(same, "same function object for RegExp.input's getter on every thread");
