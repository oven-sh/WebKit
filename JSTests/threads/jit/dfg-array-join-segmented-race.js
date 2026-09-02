//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--verifyConcurrentButterfly=1", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=50", "--osrExitCountForReoptimization=100000", "--osrExitCountForReoptimizationFromLoop=100000")
//@ runDefault()
//@ runDefault("--useFTLJIT=0")
//@ threadsRequireGILOff
// An optimized a.join("") on an Int32 array loads the butterfly, then calls an
// operation that loads it again and joins from it. A push from another thread
// past the vector length segments the array in place. If that lands between
// the two loads, the operation must see the segmented word and take the
// generic join, and it must not read past the vector of the butterfly it loaded.
//
// Each array segments once, and each segmented array costs the compiled join
// an exit, so the exit limits are raised. The reader joins one array until the
// writer has pushed to it, and the writer starts once the reader is joining it.
// The reader's loops have caps.

load("../harness.js", "caller relative");

// With the GIL on, the other thread runs only while this one blocks, so the
// race cannot happen, and the reader's loops stop early.
const gilOn = $vm.useThreadGIL();

function makeInt(i) { return [i, i + 1, i + 2, i + 3]; }
noInline(makeInt);
// An array made by code that was compiled after the priming push below does
// not segment on a push from another thread, so the shared arrays come from a
// function that is never compiled by the DFG.
function makeShared(i) { return [i, i + 1, i + 2, i + 3]; }
noDFG(makeShared);

function expected(a) {
    let s = "";
    for (let i = 0; i < a.length; ++i)
        s += a[i];
    return s;
}

// The first push from another thread fires watchpoints on the array structure,
// which throws away code compiled before it.
{
    const a = makeInt(0);
    new Thread(() => { for (let i = 0; i < 8; ++i) a.push(i); }).join();
}

const rounds = 2;
const arraysPerRound = 64;
const pushes = 8;
// The writer takes its data as arguments. A closure that captured them
// would share one executable across rounds.
function writer(arrays, state) {
    for (let k = 0; k < arrays.length; ++k) {
        while (Atomics.load(state, "reading") <= k) { }
        for (let i = 0; i < pushes; ++i)
            arrays[k].push(7);
        Atomics.store(state, "written", k + 1);
    }
}

for (let round = 0; round < rounds; ++round) {
    // The exit profile is shared by functions with the same source.
    const join = new Function("a", "return a.join(''); // round " + round);
    noInline(join);
    for (let n = 0; n < 100000 && !numberOfDFGCompiles(join); ++n) {
        const a = makeInt(n & 7);
        if (join(a) !== expected(a))
            throw new Error("warm-up join");
    }

    const arrays = [];
    const before = [];
    for (let k = 0; k < arraysPerRound; ++k) {
        arrays.push(makeShared(k));
        before.push(join(arrays[k]));
    }
    const state = { reading: 0, written: 0 };
    const other = new Thread(writer, arrays, state);

    for (let k = 0; k < arraysPerRound; ++k) {
        const a = arrays[k];
        for (let n = 0; Atomics.load(state, "written") <= k && n < (gilOn ? 3 : 2000); ++n) {
            const s = join(a);
            // The pushes only append 7s.
            if (s.slice(0, before[k].length) !== before[k] || !/^7*$/.test(s.slice(before[k].length)))
                throw new Error("bad join " + s + " in round " + round);
            if (n === 2)
                Atomics.store(state, "reading", k + 1);
        }
        Atomics.store(state, "reading", k + 1);
    }
    other.join();
    for (let k = 0; k < arraysPerRound; ++k)
        shouldBe(join(arrays[k]), before[k] + "7".repeat(pushes));
}
