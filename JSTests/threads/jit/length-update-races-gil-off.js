//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// SPEC-jit §5.5 length updates (history §46; AUDIT R9-22). Generated code
// (LLInt, Baseline, DFG, FTL) raised a published array's length with a plain
// store on a hole store and on push. GIL off, a thread that had read the old
// length could store a smaller one over a racing thread's CAS-max bump, hiding
// that thread's element behind the length. The element was there: raising the
// length again showed it. Part one: four threads fill disjoint interleaved
// indices of one flat array; before the fix rounds lost an element with the
// JIT on - an outcome no interleaving of the stores allows. Part two: the
// array's owner pushes while three threads store into holes above it; the
// owner's push stored its length plainly too. (Concurrent pushes onto one
// array are not checked: a push reads the length and stores at it, so two
// pushers may write the same index; the fix makes a push's length update
// monotone, not the push atomic.) GIL on and flag off the race cannot happen.

if (typeof Thread !== "function" || typeof $vm === "undefined") {
    print("SKIP: needs Thread and $vm");
    quit(0);
}
load("../harness.js", "caller relative");

const THREADS = 4;
const PER = 15;
const N = THREADS * PER;
const ROUNDS = 400;
// Spinning holds the GIL when it is on, so the barrier is GIL off only.
const gilOff = !$vm.useThreadGIL();

function barrier(gate) {
    Atomics.add(gate, 0, 1);
    if (gilOff) {
        while (Atomics.load(gate, 0) < THREADS) { }
    }
}

function fillDisjoint(a, gate, t) {
    barrier(gate);
    for (let k = 0; k < PER; ++k) {
        const i = t + k * THREADS;
        a[i] = "e" + i;
    }
}

for (let round = 0; round < ROUNDS; ++round) {
    // A flat array with a vector of at least 64 and length 0: every store below is a hole store, none grows.
    const a = [];
    for (let i = 0; i < 64; ++i)
        a[i] = "x";
    a.length = 0;
    const gate = new Int32Array(new SharedArrayBuffer(4));
    joinAll(spawnN(THREADS, (t) => fillDisjoint(a, gate, t)));
    if (a.length !== N) {
        const length = a.length;
        a.length = N;
        const missing = [];
        for (let i = 0; i < N; ++i) {
            if (a[i] !== "e" + i)
                missing.push(i);
        }
        throw new Error(`hole stores, round ${round}: length ${length}, expected ${N}; after raising the length, missing ${JSON.stringify(missing)}`);
    }
}

// Part two: the owner pushes while three other threads store into holes above it. The owner's push raised the length
// with a plain store (in the optimizing tiers on every push once the array's set had fired, and in the runtime's
// pushInline), which could lower a foreign raise. Nothing here lowers the length, so it must end above every foreign
// index; a foreign index holds its value or a later owner push's.
const FOREIGN = 3;
const HIGH = 40;
const FOREIGN_PER = 8;
const OWNER_PUSHES = 30;
const maxForeignIndex = HIGH + FOREIGN * FOREIGN_PER - 1;

function fillHigh(a, gate, t) {
    barrier(gate);
    for (let k = 0; k < FOREIGN_PER; ++k) {
        const i = HIGH + t + k * FOREIGN;
        a[i] = "f" + i;
    }
}

function ownerPushes(a, gate) {
    barrier(gate);
    for (let k = 0; k < OWNER_PUSHES; ++k)
        a.push("o" + k);
}

for (let round = 0; round < ROUNDS; ++round) {
    // Vector of at least 128, length 0: neither the pushes nor the foreign stores grow it.
    const a = [];
    for (let i = 0; i < 128; ++i)
        a[i] = "x";
    a.length = 0;
    const gate = new Int32Array(new SharedArrayBuffer(4));
    const threads = spawnN(FOREIGN, (t) => fillHigh(a, gate, t));
    ownerPushes(a, gate);
    joinAll(threads);
    if (a.length <= maxForeignIndex)
        throw new Error(`owner pushes, round ${round}: length ${a.length}, below foreign index ${maxForeignIndex}`);
    for (let t = 0; t < FOREIGN; ++t) {
        for (let k = 0; k < FOREIGN_PER; ++k) {
            const i = HIGH + t + k * FOREIGN;
            const v = a[i];
            if (v !== "f" + i && !(typeof v === "string" && v[0] === "o"))
                throw new Error(`owner pushes, round ${round}: index ${i} holds ${v}`);
        }
    }
}

print("PASS");
