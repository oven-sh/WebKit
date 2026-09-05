//@ requireOptions("--useJSThreads=1")
// A sloppy-mode function whose parameters are captured by a closure gets a
// scoped `arguments` object. Its `length`, `callee` and `Symbol.iterator`
// properties are made on first write or delete of one of them. With the GIL
// off two threads can do that first write together on a shared arguments
// object; each must find the properties made once, and a delete on one thread
// must not come back.
load("../harness.js", "caller relative");

const THREADS = 4;
const ROUNDS = 2000;

function makeScoped(a, b, c) {
    // The closure captures a parameter, so `arguments` is a ScopedArguments.
    const f = () => a;
    return arguments;
}

{
    const all = [];
    for (let i = 0; i < ROUNDS; ++i)
        all.push(makeScoped(i, i + 1, i + 2));
    const gate = { go: 0 };
    const threads = spawnN(THREADS, (t) => {
        while (Atomics.load(gate, "go") === 0) { }
        let n = 0;
        for (let i = 0; i < ROUNDS; ++i) {
            const args = all[i];
            // The first store of `length` materializes length, callee and
            // Symbol.iterator on the object.
            args.length = 10 + t;
            if (typeof args.callee === "function")
                ++n;
        }
        return n;
    });
    Atomics.store(gate, "go", 1);
    for (const n of joinAll(threads))
        shouldBe(n, ROUNDS, "callee seen by a thread");
    for (let i = 0; i < ROUNDS; ++i) {
        const args = all[i];
        shouldBeTrue(args.length >= 10 && args.length < 10 + THREADS, "length of arguments " + i);
        shouldBe(args.callee, makeScoped, "callee of arguments " + i);
        shouldBe(args[0], i, "element 0 of arguments " + i);
        shouldBe(typeof args[Symbol.iterator], "function", "Symbol.iterator of arguments " + i);
    }
}

{
    // One thread deletes `callee` while the others store `length`.
    const all = [];
    for (let i = 0; i < ROUNDS; ++i)
        all.push(makeScoped(i, i + 1, i + 2));
    const gate = { go: 0 };
    const storers = spawnN(THREADS - 1, (t) => {
        while (Atomics.load(gate, "go") === 0) { }
        for (let i = 0; i < ROUNDS; ++i)
            all[i].length = 7;
        return ROUNDS;
    });
    const deleter = new Thread(() => {
        while (Atomics.load(gate, "go") === 0) { }
        let n = 0;
        for (let i = 0; i < ROUNDS; ++i) {
            if (delete all[i].callee)
                ++n;
        }
        return n;
    });
    Atomics.store(gate, "go", 1);
    joinAll(storers);
    shouldBe(deleter.join(), ROUNDS, "deletes");
    for (let i = 0; i < ROUNDS; ++i) {
        shouldBe(all[i].length, 7, "length after the stores, arguments " + i);
        shouldBeFalse("callee" in all[i], "callee came back, arguments " + i);
    }
}
