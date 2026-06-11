//@ requireOptions("--useJSThreads=1")
// semantics/symbol-registry-cross-thread.js — Symbol.for() identity across
// threads. The global symbol registry is VM-wide shared state: Symbol.for(s)
// for the same character sequence must return the SAME symbol (===) on every
// thread, no matter which thread registered it first, which string
// construction path produced the key (literal, rope, fromCharCode, slice),
// or how many threads race the first registration of a key.
load("../harness.js", "caller relative");

const N = 48;
const THREADS = 4;

function keyVariant(i, variant) {
    const base = "semRegKey_" + i;
    switch (variant % 3) {
    case 0:
        return base; // literal-derived rope
    case 1: {
        let s = "";
        for (let j = 0; j < base.length; ++j)
            s += String.fromCharCode(base.charCodeAt(j)); // char-by-char build
        return s;
    }
    default:
        return ("@@" + base + "@@").slice(2, 2 + base.length); // slice of a backing store
    }
}

// --- Race the FIRST registration: all threads call Symbol.for on the same
// fresh keys simultaneously, each through a different string path, and
// return their symbols. All must be identical per key.
{
    const gate = { ready: 0, go: 0 };
    const racers = spawnN(THREADS, t => {
        Atomics.add(gate, "ready", 1);
        while (Atomics.load(gate, "go") === 0)
            Atomics.wait(gate, "go", 0, 2);
        const syms = [];
        for (let i = 0; i < N; ++i)
            syms.push(Symbol.for(keyVariant(i, t)));
        return syms;
    });
    waitUntil(() => Atomics.load(gate, "ready") === THREADS);
    Atomics.store(gate, "go", 1);
    Atomics.notify(gate, "go", Infinity);
    const all = joinAll(racers);
    for (let i = 0; i < N; ++i) {
        const main = Symbol.for(keyVariant(i, 2)); // main thread's own lookup
        for (let t = 0; t < THREADS; ++t) {
            if (all[t][i] !== main)
                throw new Error("thread " + t + " got a different symbol for key " + i + " — registry split");
        }
        shouldBe(Symbol.keyFor(main), "semRegKey_" + i, "keyFor round-trips key " + i);
    }
}

// --- A symbol registered on a spawned thread is the same object on main,
// and usable as a property key across threads (atomized-symbol identity in
// the property table).
{
    const carrier = {};
    const fromThread = new Thread(o => {
        const s = Symbol.for("semReg_propKey");
        o[s] = "written-by-thread";
        return s;
    }, carrier).join();
    const onMain = Symbol.for("semReg_" + "propKey"); // rope-built key
    shouldBe(fromThread, onMain, "registered symbol identical across threads");
    shouldBe(carrier[onMain], "written-by-thread", "property under the shared symbol visible via main's lookup");
    shouldBe(Object.getOwnPropertySymbols(carrier).length, 1);
    shouldBe(Object.getOwnPropertySymbols(carrier)[0], onMain);
}

// --- Unregistered symbols must NOT alias the registry: Symbol(desc) made on
// one thread is distinct from Symbol.for(desc) anywhere, and keyFor on it is
// undefined on every thread.
{
    const plain = new Thread(() => Symbol("semReg_unregistered")).join();
    shouldBeTrue(plain !== Symbol.for("semReg_unregistered"), "unregistered symbol distinct from registry entry");
    shouldBe(Symbol.keyFor(plain), undefined, "keyFor(unregistered) is undefined on main");
    shouldBe(new Thread(s => Symbol.keyFor(s), plain).join(), undefined, "keyFor(unregistered) is undefined on a thread");
    // Well-known symbols are also a single identity everywhere.
    shouldBe(new Thread(() => Symbol.iterator).join(), Symbol.iterator, "well-known symbol identity across threads");
}

// --- Churn: threads repeatedly re-look-up an overlapping key window while
// new keys keep being registered; identity must never wobble.
{
    const baseline = [];
    for (let i = 0; i < N; ++i)
        baseline.push(Symbol.for(keyVariant(i, 0)));
    const churners = spawnN(THREADS, t => {
        for (let r = 0; r < 200; ++r) {
            const i = (r * 7 + t) % N;
            if (Symbol.for(keyVariant(i, r)) !== Symbol.for(keyVariant(i, r + 1)))
                throw new Error("thread " + t + " round " + r + ": registry returned two symbols for key " + i);
            Symbol.for("semRegChurn_" + t + "_" + r); // fresh registration pressure
        }
        return true;
    });
    for (const ok of joinAll(churners))
        shouldBe(ok, true);
    for (let i = 0; i < N; ++i)
        shouldBe(Symbol.for(keyVariant(i, 1)), baseline[i], "key " + i + " stable after churn");
}

// WOULD-FAIL-IF: the global symbol registry loses cross-thread uniqueness —
// a racy first registration where two threads both miss the lookup and both
// insert (two live symbols for one key: the all[t][i] !== main compare
// fails), per-thread/per-VMLite registry shards that don't share entries
// (main's Symbol.for can't see a thread's registration: property lookup
// under the symbol returns undefined and the identity compare fails), key
// comparison done by atom pointer instead of characters (the three string
// construction paths produce distinct StringImpls, so a pointer-keyed
// registry returns different symbols per path), or registration churn
// invalidating existing entries (baseline[i] stops matching). Every check is
// an exact === on symbol identity, so any of these splits fails loudly.
