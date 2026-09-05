//@ requireOptions("--useJSThreads=1")
// A function's `prototype` property is created on first use. With the GIL
// off, several threads can use it first at the same time: `new F()` on each of
// them reads F.prototype to set up F's allocation profile. Each thread used to
// create its own prototype object and store it, and a later store replaced an
// earlier one, so objects that a thread had already made were not
// `instanceof F` and did not see what was later put on F.prototype. The first
// store wins now, and every thread uses that object.
//
// Also covers a store to F.prototype that races with the first read: the read
// must not replace the stored value with a fresh default object.
load("../harness.js", "caller relative");

const THREADS = 4;
const ROUNDS = 1500;

{
    const ctors = [];
    for (let i = 0; i < ROUNDS; ++i)
        ctors.push(new Function("a", "this.x = a;"));
    const gate = { go: 0 };
    const threads = spawnN(THREADS, () => {
        while (Atomics.load(gate, "go") === 0) { }
        const made = [];
        for (let i = 0; i < ROUNDS; ++i)
            made.push(new ctors[i](i));
        return made;
    });
    Atomics.store(gate, "go", 1);
    const made = joinAll(threads);
    for (let i = 0; i < ROUNDS; ++i) {
        const proto = ctors[i].prototype;
        shouldBe(proto.constructor, ctors[i], "prototype.constructor of function " + i);
        for (let t = 0; t < THREADS; ++t) {
            const o = made[t][i];
            shouldBe(Object.getPrototypeOf(o), proto, "prototype of the object thread " + t + " made with function " + i);
            shouldBeTrue(o instanceof ctors[i], "instanceof, thread " + t + ", function " + i);
        }
    }
}

{
    const ctors = [];
    const replacements = [];
    for (let i = 0; i < ROUNDS; ++i) {
        ctors.push(new Function("a", "this.x = a;"));
        replacements.push({ replaced: i });
    }
    const gate = { go: 0 };
    // One thread stores F.prototype while the others read it for the first time.
    const readers = spawnN(THREADS - 1, () => {
        while (Atomics.load(gate, "go") === 0) { }
        let seen = 0;
        for (let i = 0; i < ROUNDS; ++i) {
            if (typeof ctors[i].prototype === "object")
                ++seen;
        }
        return seen;
    });
    const writer = new Thread(() => {
        while (Atomics.load(gate, "go") === 0) { }
        for (let i = 0; i < ROUNDS; ++i)
            ctors[i].prototype = replacements[i];
        return ROUNDS;
    });
    Atomics.store(gate, "go", 1);
    for (const seen of joinAll(readers))
        shouldBe(seen, ROUNDS, "prototypes a reader saw");
    shouldBe(writer.join(), ROUNDS, "stores the writer made");
    for (let i = 0; i < ROUNDS; ++i)
        shouldBe(ctors[i].prototype, replacements[i], "F.prototype after the store, function " + i);
}
