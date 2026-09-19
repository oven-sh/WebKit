//@ runDefault("--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForFTLOptimizeAfterWarmUp=50")
//@ runDefault("--evacuateAuxiliaryBlocksAfterEveryFullCollection=1", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForFTLOptimizeAfterWarmUp=50")

// Compiler threads read the storage of constant objects (to fold loads of their properties and elements) while the
// mutator evacuates it and, with evacuateAuxiliaryBlocksAfterEveryFullCollection, scribbles over the old copies. They are
// parked at a safepoint while cells move.

// The constant objects below, and garbage of the same shapes and storage sizes.
function makeHolder(seed) {
    const object = {};
    for (let i = 0; i < 20; ++i)
        object["k" + i] = seed * 100 + i;
    object.array = [];
    object.doubles = [];
    for (let i = 0; i < 30; ++i) {
        object.array.push(seed + i);
        object.doubles.push(seed + i + 0.5);
    }
    return object;
}
noInline(makeHolder);
const holders = [];
for (let h = 0; h < 20; ++h) {
    const object = makeHolder(h);
    holders.push(object);
    globalThis["H" + h] = object;
}
// Without the testing option an evacuation only takes blocks that it can free: two survivors, each followed by more than a
// block's worth of garbage of the same sizes, make the blocks that hold the storage of the constant objects sparse again.
// (125 for MarkedBlocks of 16 KB; a block is larger where pages can be: 64 KB on Linux arm64.)
const garbagePerSurvivor = 125 * $vm.markedBlockStatistics().blockSize / (16 * 1024);
let survivors = [];
function garbage() {
    survivors = [];
    for (let s = 0; s < 2; ++s) {
        survivors.push(makeHolder(s));
        for (let i = 0; i < garbagePerSurvivor; ++i)
            makeHolder(-i);
    }
}
noInline(garbage);
// These two in frames of their own, so that the constant objects are not on the stack (which would keep their storage in place).
function expectedResult(h) {
    let expected = h;
    for (let i = 0; i < 50; ++i) {
        const object = holders[h];
        expected += object.k11 + object.k19 + object["k" + h] + object.array[i % 30] + object.array.length + object.doubles[1] + holders[(h + 7) % 20].k15;
    }
    return expected;
}
noInline(expectedResult);
function storageOf(object) { return /butterfly (0x[0-9a-f]+)/.exec(describe(object))[1]; }
function storageOfConstants(h) {
    const result = [];
    for (const object of [holders[h], holders[(h + 7) % 20]])
        result.push(storageOf(object), storageOf(object.array), storageOf(object.doubles));
    return result;
}
noInline(storageOfConstants);
let evacuations = 0, evacuationsThatMovedTheConstants = 0;
// One generation per constant object: a new function that the compiler threads compile while its loop runs.
const generations = Math.max(10, Math.min(20, Math.floor(testLoopCount / 200)));
for (let h = 0; h < generations; ++h) {
    const f = new Function("n", `let s = 0; const o = H${h}; for (let i = 0; i < n; ++i) { s += o.k11 + o.k19 + o.k${h} + o.array[i % 30] + o.array.length + o.doubles[1] + H${(h + 7) % 20}.k15; } return s + ${h};`);
    const expected = expectedResult(h);
    for (let repeat = 0; repeat < 150; ++repeat) {
        const result = f(50);
        if (result !== expected)
            throw new Error("generation " + h + ", repeat " + repeat + ": " + result + ", expected " + expected);
        if (repeat % 25 === 24) {
            const before = storageOfConstants(h);
            garbage();
            gc();
            $vm.evacuateAuxiliaryBlocks(1);
            const after = storageOfConstants(h);
            evacuations++;
            if (before.every((storage, i) => storage !== after[i]))
                evacuationsThatMovedTheConstants++;
        }
    }
}
// The storage that the compiler threads may be reading has to move, or this tests nothing.
if (evacuationsThatMovedTheConstants < evacuations - generations)
    throw new Error("only " + evacuationsThatMovedTheConstants + " of " + evacuations + " evacuations moved the storage of the constant objects that the function of their generation uses");
