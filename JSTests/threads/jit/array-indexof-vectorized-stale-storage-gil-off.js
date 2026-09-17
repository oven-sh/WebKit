//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--countJSThreadsCounters=1")
//@ threadsRequireGILOff
// SPEC-jit history §39 and §47 (AUDIT R10-6). GIL off, generated code hands
// the indexOf/includes search operations (the vectorized search, taken when 32
// or more elements remain) the flat butterfly it has just loaded, and the
// operation read the array's publicLength again from that butterfly. A foreign
// thread that stores past the end of a flat array another thread owns converts
// it to segmented storage in place and raises its length in the same step; the
// old flat header's length slot is the one that is raised (it aliases the
// segmented array's length), so an operation that was handed the flat pointer
// just before the conversion scanned `newLength - index` elements from an
// allocation that holds only `vectorLength`: past its end, into whatever the
// allocator had placed after it. The operations now clamp the length to the
// storage's own vectorLength, as the FTL's inline paths do (§39).
//
// The test makes the out-of-bounds scan observable without a sanitizer. The
// arrays searched never hold the needle; thousands of arrays of the same size
// allocated around them hold nothing else, so a scan past the end of one finds
// the needle in a neighbour. A first foreign write to yet another array of the
// same structure fires the structure's thread-local sets up front (one stop),
// so the later conversions take no stop and compiled searches survive them.
// Then, array by array, the main thread searches one array with that array's
// own compiled search function while a second thread converts that very array
// by storing past its capacity; each conversion is one chance for the race, and
// there are hundreds (a search that starts after the conversion exits its
// compiled code once, which is why every array has its own function). A search
// that answers anything but "not found" read outside its array. With
// --countJSThreadsCounters the clamp's counter shows how many operations met a
// stale length (0 on a build without the clamp, which has no such counter).
load("../harness.js", "caller relative");

const NEEDLE = 7777.25;
const LENGTH = 64;
const TARGETS = 200;
const FOREIGN_INDEX = LENGTH * 4; // past any push slack of a 64-element array: the store converts and grows in one step
const gate = new Int32Array(new SharedArrayBuffer(16)); // [0] go, [1] number of targets released so far

function makeArray(fill) {
    const a = [];
    for (let i = 0; i < LENGTH; ++i)
        a.push(fill === undefined ? i + 0.5 : fill);
    return a;
}

// Targets interleaved with neighbours full of the needle.
const spray = [];
const targets = [];
for (let i = 0; i < TARGETS; ++i) {
    for (let j = 0; j < 8; ++j)
        spray.push(makeArray(NEEDLE));
    targets.push(makeArray());
    for (let j = 0; j < 8; ++j)
        spray.push(makeArray(NEEDLE));
}

// Fire the structure's sets once, on an array that is not searched.
const decoy = makeArray();
new Thread(() => { decoy[1] = 1.5; decoy[FOREIGN_INDEX] = 0.5; }).join();

// One search function per target, each compiled (DFG or FTL) on its own array.
// Eight searches per call: eight windows between a storage load and the
// operation's length read for one check of the gate.
const probeSource = `
    let bad = 0;
    if (a.indexOf(n) !== -1) bad = 1; if (a.includes(n)) bad = 2;
    if (a.indexOf(n) !== -1) bad = 3; if (a.includes(n)) bad = 4;
    if (a.indexOf(n) !== -1) bad = 5; if (a.includes(n)) bad = 6;
    if (a.indexOf(n) !== -1) bad = 7; if (a.includes(n)) bad = 8;
    return bad;`;
const probes = [];
for (let k = 0; k < TARGETS; ++k) {
    // The trailing comment makes each source distinct: functions built from one
    // source share their code, compile state and exit profile.
    const probe = new Function("a", "n", probeSource + " // probe " + k);
    noInline(probe);
    probes.push(probe);
}
// Warm every probe on its own array until the DFG (or FTL) has compiled it;
// the searches that call the operations are the optimizing tiers'.
let compiled = 0;
for (let round = 0; round < 400 && compiled < TARGETS; ++round) {
    compiled = 0;
    for (let k = 0; k < TARGETS; ++k) {
        if (numberOfDFGCompiles(probes[k])) {
            ++compiled;
            continue;
        }
        for (let i = 0; i < 100; ++i) {
            if (probes[k](targets[k], NEEDLE))
                throw new Error("warm-up found the needle");
        }
    }
}

const foreign = new Thread(() => {
    while (!Atomics.load(gate, 0)) { }
    let seed = 12345;
    for (let k = 0; k < TARGETS; ++k) {
        // A short pseudo-random spin so the conversion lands anywhere in the
        // owner's search loop; then release the owner from this array and
        // convert it right away, so the owner's search in flight, if any,
        // overlaps the conversion and no later search of it starts.
        seed = (seed * 1103515245 + 12345) & 0x7fffffff;
        for (let spin = 20000 + seed % 60000; spin > 0; --spin) { }
        Atomics.store(gate, 1, k + 1);
        targets[k][FOREIGN_INDEX] = k + 0.5; // converts targets[k] to segmented storage and raises its length to FOREIGN_INDEX + 1
    }
});

let bad = 0;
let badTarget = -1;
Atomics.store(gate, 0, 1);
for (let k = 0; k < TARGETS && !bad; ++k) {
    const a = targets[k];
    const probe = probes[k];
    while (Atomics.load(gate, 1) <= k) {
        bad = probe(a, NEEDLE);
        if (bad) {
            badTarget = k;
            break;
        }
    }
}
foreign.join();
if (bad)
    throw new Error("a search of an array that never held the needle found it (check " + bad + ", target " + badTarget + ")");
for (let k = 0; k < TARGETS; ++k) {
    const a = targets[k];
    if (a.length !== FOREIGN_INDEX + 1 || a[FOREIGN_INDEX] !== k + 0.5 || a[LENGTH - 1] !== LENGTH - 1 + 0.5 || a[LENGTH] !== undefined)
        throw new Error("target " + k + ": length " + a.length + ", [" + FOREIGN_INDEX + "] " + a[FOREIGN_INDEX]);
}
if (typeof AMPLIFY_VERBOSE !== "undefined")
    print("clamped searches " + $vm.jsThreadsCounter("searchOperationClampedStaleStorageGILOff"));
