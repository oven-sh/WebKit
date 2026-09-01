//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// barrier-storm.js — GR1 experimenter amplifier (scratch, not a corpus test).
// Goal: maximize concurrent Heap::addToRememberedSet traffic on the SHARED
// mutator mark stack from N mutators, under constant eden GC (run with
// --forceRAMSize=4194304). The C++ side (BUGHUNT-GR1 instrumentation) counts
// appends, concurrent-entry overlaps, per-append size anomalies, and
// appended-vs-drained shadow mismatches. This test has no JS-level oracle
// beyond completing; the detector verdict is in the [BUGHUNT-GR1] lines.

load("../../../../JSTests/threads/harness.js", "caller relative");

const THREADS = 4;
const N = 2048;       // old objects per slot-group (shared, all threads write all)
const ROUNDS = 300;

// Build per-thread populations (owner-only writes: no SW-bit Class-A
// traffic; the write barrier still lands on the ONE shared mutator mark
// stack from all 5 mutators). Aged by allocation pressure below.
const groups = [];
for (let t = 0; t <= THREADS; ++t) {
    const arr = [];
    for (let i = 0; i < N; ++i)
        arr.push({ f0: null, f1: null, f2: null, f3: null, f4: null });
    groups.push(arr);
}

// Force aging: a couple of collections via allocation pressure.
let garbage = null;
for (let k = 0; k < 20000; ++k)
    garbage = { a: k, b: [k, k + 1, k + 2] };

const ready = { count: 0 };

function threadBody(slot) {
    Atomics.add(ready, "count", 1);
    waitUntil(() => Atomics.load(ready, "count") > THREADS, 30000);
    const own = groups[slot];
    let g = null;
    for (let r = 0; r < ROUNDS; ++r) {
        const v = [r];  // young cell value => store barrier fires on old owner
        for (let i = 0; i < N; ++i)
            own[i].f0 = v;
        for (let j = 0; j < 512; ++j)
            g = { a: r, b: j }; // eden pressure => frequent GCs
        if (!(r % 32))
            sleepMs(1);
    }
    return g === null ? 1 : 2;
}

const threads = spawnN(THREADS, threadBody);
const mainResult = threadBody(THREADS);
const results = joinAll(threads);
shouldBeTrue(mainResult > 0);
for (const r of results)
    shouldBeTrue(r > 0);
print("barrier-storm: PASS");
