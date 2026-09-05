//@ requireOptions("--useJSThreads=1")
// A function's own `length` and `name` properties are created on first use.
// With the GIL off several threads can read them first at the same time on a
// shared function; each must get the function's real length and name, not
// the defaults from Function.prototype, and a value defined over one of them
// on one thread must not be replaced by the original on another.
load("../harness.js", "caller relative");

const THREADS = 4;
const ROUNDS = 2000;

function makeFunctions() {
    const fns = [];
    for (let i = 0; i < ROUNDS; ++i)
        fns.push(new Function("a", "b", "c", "return " + i + ";"));
    return fns;
}

{
    const fns = makeFunctions();
    const gate = { go: 0 };
    const threads = spawnN(THREADS, (t) => {
        while (Atomics.load(gate, "go") === 0) { }
        let wrongLength = 0;
        let wrongName = 0;
        for (let i = 0; i < ROUNDS; ++i) {
            const f = fns[i];
            // Alternate the order, so that one thread makes `length` first
            // while another makes `name` first on the same function.
            if ((i + t) & 1) {
                if (f.length !== 3)
                    ++wrongLength;
                if (f.name !== "anonymous")
                    ++wrongName;
            } else {
                if (f.name !== "anonymous")
                    ++wrongName;
                if (f.length !== 3)
                    ++wrongLength;
            }
        }
        return [wrongLength, wrongName];
    });
    Atomics.store(gate, "go", 1);
    const results = joinAll(threads);
    for (let t = 0; t < THREADS; ++t) {
        shouldBe(results[t][0], 0, "wrong lengths seen by thread " + t);
        shouldBe(results[t][1], 0, "wrong names seen by thread " + t);
    }
    for (let i = 0; i < ROUNDS; ++i) {
        shouldBe(fns[i].length, 3, "length of function " + i);
        shouldBe(fns[i].name, "anonymous", "name of function " + i);
        const own = Object.getOwnPropertyNames(fns[i]).filter((k) => k === "length" || k === "name");
        shouldBe(own.length, 2, "own length/name properties of function " + i);
    }
}

{
    // One thread defines `length` while the others read it for the first time.
    const fns = makeFunctions();
    const gate = { go: 0 };
    const readers = spawnN(THREADS - 1, () => {
        while (Atomics.load(gate, "go") === 0) { }
        let bad = 0;
        for (let i = 0; i < ROUNDS; ++i) {
            const n = fns[i].length;
            if (n !== 3 && n !== 42)
                ++bad;
        }
        return bad;
    });
    const writer = new Thread(() => {
        while (Atomics.load(gate, "go") === 0) { }
        for (let i = 0; i < ROUNDS; ++i)
            Object.defineProperty(fns[i], "length", { value: 42 });
        return ROUNDS;
    });
    Atomics.store(gate, "go", 1);
    for (const bad of joinAll(readers))
        shouldBe(bad, 0, "lengths that were neither the original nor the defined value");
    shouldBe(writer.join(), ROUNDS, "defines");
    for (let i = 0; i < ROUNDS; ++i)
        shouldBe(fns[i].length, 42, "length after the define, function " + i);
}
