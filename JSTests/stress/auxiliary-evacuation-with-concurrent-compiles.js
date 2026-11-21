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

// An evacuation leaves storage in place when a word of the stack refers to it or to its object, and one cell that stays
// keeps its whole block in place: here, the storage of all the constant objects. The words it scans include those that
// its own native frames lie over and never write, i.e. what earlier, deeper calls left there. In a build without
// optimization those frames reach 6 KB below their caller, and which calls wrote there last depends on how far the
// compiler threads have got: on a loaded machine one such word kept the constants in place for 27 evacuations in a row.
// So everything that has a constant object, its storage or garbage in a frame or a register runs this many frames below
// the code that collects and evacuates (a frame of descend() is 72 bytes or more), and leaves its results in variables:
// a returned array would be in every frame on the way up.
const framesBelowTheEvacuation = 1000;
function descend(frames, work, argument) {
    if (frames)
        descend(frames - 1, work, argument);
    else
        work(argument);
}
noInline(descend);
function farBelow(work, argument) { descend(framesBelowTheEvacuation, work, argument); }
noInline(farBelow);

// More than a block's worth of objects. (125 for MarkedBlocks of 16 KB; a block is larger where pages can be: 64 KB on
// Linux arm64.)
const garbagePerSurvivor = 125 * $vm.markedBlockStatistics().blockSize / (16 * 1024);

// The block that storage of this size goes to first already has storage in it that the VM allocated while it started (of
// the Math object, of constructors), and a word that refers to one of those objects for as long as the program runs keeps
// that block in place (in one build, a word that the frame of Interpreter::executeProgram lies over). So more than a
// block's worth of storage of the same size goes there first, and the constant objects get blocks of their own. The
// filler is a holder without its arrays: the first arrays of a program are precise allocations, and those stay the
// constant objects', so that an evacuation looks their owners up both ways.
function makeFiller(seed) {
    const object = {};
    for (let i = 0; i < 20; ++i)
        object["k" + i] = seed * 100 + i;
    object.array = null;
    object.doubles = null;
    return object;
}
noInline(makeFiller);
const holders = [];
let filler = [];
function makeHolders() {
    for (let i = 0; i < garbagePerSurvivor; ++i)
        filler.push(makeFiller(i));
    for (let h = 0; h < 20; ++h) {
        const object = makeHolder(h);
        holders.push(object);
        globalThis["H" + h] = object;
    }
}
noInline(makeHolders);
farBelow(makeHolders);
filler = null;
// Without the testing option an evacuation only takes blocks that it can free: two survivors, each followed by more than a
// block's worth of garbage of the same sizes, make the blocks that hold the storage of the constant objects sparse again.
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

// One generation per constant object: a new function that the compiler threads compile while its loop runs.
let f, expected;
function startGeneration(h) {
    f = new Function("n", `let s = 0; const o = H${h}; for (let i = 0; i < n; ++i) { s += o.k11 + o.k19 + o.k${h} + o.array[i % 30] + o.array.length + o.doubles[1] + H${(h + 7) % 20}.k15; } return s + ${h};`);
    expected = expectedResult(h);
}
noInline(startGeneration);
let storageBefore;
function runUntilTheNextEvacuation(h) {
    for (let repeat = 0; repeat < 25; ++repeat) {
        const result = f(50);
        if (result !== expected)
            throw new Error("generation " + h + ", repeat " + repeat + " since the last evacuation: " + result + ", expected " + expected);
    }
    storageBefore = storageOfConstants(h);
}
noInline(runUntilTheNextEvacuation);
let evacuations = 0, evacuationsThatMovedTheConstants = 0, lastEvacuation;
function countTheEvacuation(h) {
    const storageAfter = storageOfConstants(h);
    evacuations++;
    if (storageBefore.every((storage, i) => storage !== storageAfter[i]))
        evacuationsThatMovedTheConstants++;
}
noInline(countTheEvacuation);

const generations = Math.max(10, Math.min(20, Math.floor(testLoopCount / 200)));
for (let h = 0; h < generations; ++h) {
    farBelow(startGeneration, h);
    for (let evacuation = 0; evacuation < 6; ++evacuation) {
        farBelow(runUntilTheNextEvacuation, h);
        farBelow(garbage);
        gc();
        lastEvacuation = $vm.evacuateAuxiliaryBlocks(1);
        farBelow(countTheEvacuation, h);
    }
}
// The storage that the compiler threads may be reading has to move, or this tests nothing.
if (evacuationsThatMovedTheConstants < evacuations - generations)
    throw new Error("only " + evacuationsThatMovedTheConstants + " of " + evacuations + " evacuations moved the storage of the constant objects that the function of their generation uses; the last one: " + JSON.stringify(lastEvacuation));
