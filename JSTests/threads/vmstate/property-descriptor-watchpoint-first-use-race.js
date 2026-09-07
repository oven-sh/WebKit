//@ requireOptions("--useJSThreads=1")
// The global object sets up its "plain property descriptor" fast path the
// first time Object.defineProperty (or defineProperties, Reflect.defineProperty)
// is called with a descriptor object: it installs a set of watchpoints on
// Object.prototype and starts watching them. With the GIL off several threads
// can make that first call at the same time; the set-up must happen once, and
// every call must define its property.
load("../harness.js", "caller relative");

const THREADS = 6;
const ROUNDS = 200;

const gate = { go: 0 };
const threads = spawnN(THREADS, (t) => {
    while (Atomics.load(gate, "go") === 0) { }
    let defined = 0;
    for (let i = 0; i < ROUNDS; ++i) {
        const o = {};
        // Alternate the three entry points that share the set-up.
        if (i % 3 === 0)
            Object.defineProperty(o, "p" + i, { value: i, writable: true, enumerable: true, configurable: true });
        else if (i % 3 === 1)
            Object.defineProperties(o, { ["p" + i]: { value: i, enumerable: true } });
        else
            Reflect.defineProperty(o, "p" + i, { get() { return i; }, configurable: true });
        if (o["p" + i] === i)
            ++defined;
    }
    return defined;
});
Atomics.store(gate, "go", 1);
const results = joinAll(threads);
for (let t = 0; t < THREADS; ++t)
    shouldBe(results[t], ROUNDS, "properties defined by thread " + t);

// The fast path itself still works afterwards, on every thread.
const after = spawnN(THREADS, () => {
    const o = {};
    Object.defineProperty(o, "x", { value: 42, writable: false });
    return Object.getOwnPropertyDescriptor(o, "x").writable === false && o.x === 42;
});
for (const ok of joinAll(after))
    shouldBe(ok, true, "descriptor fast path after set-up");
