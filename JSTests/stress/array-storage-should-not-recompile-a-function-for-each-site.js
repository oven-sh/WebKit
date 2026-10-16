//@ skip if not $jitTests
//@ requireOptions("--useConcurrentJIT=false")
//@ runDefault
//@ runDefault("--useArrayStorageSamplesAsSpeculationFailures=false")

// Each function below uses one array at several sites, and half of its arrays are ArrayStorage arrays. Such an array
// fails the array check of the first site only. The sites after it see the array in the Baseline code that the exit
// continues in, so one jettison of the DFG code teaches all of them. Without that, each site needs a compilation of
// its own, and the reoptimization counter doubles the thresholds each time.

// Only DFG code that could still reach the FTL teaches them: see CodeBlock::tallyFrequentExitSites.
if (!$vm.useDFGJIT() || !$vm.useFTLJIT())
    quit();

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

let recordsSamples = jscOptions().useArrayStorageSamplesAsSpeculationFailures !== 0;
let count = testLoopCount * 10;

function create() {
    let array = [];
    for (let i = 0; i < 8; ++i)
        array.push(1);
    return array;
}
noInline(create);

function createFrozen() {
    return Object.freeze(create());
}

// A sparse array has the original structure for ArrayStorage.
function createSparse() {
    let array = create();
    array[200000] = 1;
    return array;
}

// So has an array that held more than 128 elements when shift() was called.
function createQueue() {
    let array = [];
    for (let i = 0; i < 129; ++i)
        array.push(1);
    while (array.length > 8)
        array.shift();
    return array;
}

function Matrix() {
    for (let i = 0; i < 8; ++i)
        this[i] = 1;
}

function createMatrix() {
    return new Matrix;
}

function createFrozenMatrix() {
    return Object.freeze(new Matrix);
}

function read(array) {
    return array[0] + array[1] + array[2] + array[3] + array[4] + array[5] + array[6] + array[7] - 4;
}

function store(array) {
    array[0] = 1;
    array[1] = 1;
    array[2] = 1;
    array[3] = 1;
    return 4;
}

function has(array) {
    return (0 in array) + (1 in array) + (2 in array) + (3 in array);
}

function readAndLength(array) {
    return array[0] + array[1] + array[2] + array.length - 7;
}

function destructure(array) {
    let [first, second, third, fourth] = array;
    return first + second + third + fourth;
}

function pushAndPop(array) {
    array.push(1);
    array.push(1);
    return array.pop() + array.pop() + 2;
}

function readInHelper(array) {
    return array[0] + array[1] + array[2] + array[3] + array[4] + array[5];
}

// The array check that fails is in the inlined helper.
function readPropertyWithInlinedHelper(holder) {
    return readInHelper(holder.array) - 2;
}

let copies = 0;

// Runs a copy of func, which has array profiles of its own, with ordinary and ArrayStorage arrays in turn.
// expectedCompiles is 2 when the first check that fails is an array check. It is 3 when a structure check fails first,
// one that was hoisted to the entry of the function or the one of a property load: no array profile says there what
// failed, so that jettison teaches nothing.
function test(func, createOrdinary, createArrayStorage, expectedCompiles, wrap = (array) => array) {
    let copy = (0, eval)(`(${func.toString()}) // ${++copies}`);
    noInline(copy);

    let ordinary = [];
    let arrayStorage = [];
    for (let i = 0; i < 16; ++i) {
        ordinary.push(createOrdinary());
        arrayStorage.push(createArrayStorage());
    }
    let ordinaryMode = $vm.indexingMode(ordinary[0]);
    shouldBe($vm.indexingMode(arrayStorage[0]), ordinaryMode.startsWith("NonArray") ? "NonArrayWithArrayStorage" : "ArrayWithArrayStorage");
    let unwrapped = ordinary;
    ordinary = ordinary.map(wrap);
    arrayStorage = arrayStorage.map(wrap);

    // The profiles see both kinds of arrays before the first compile. A full GC folds the samples into the profiles.
    for (let i = 0; i < 4; ++i) {
        shouldBe(copy(arrayStorage[i]), 4);
        fullGC();
        shouldBe(copy(ordinary[i]), 4);
        fullGC();
    }
    // The call that makes the DFG compile it has an ordinary array, so the DFG can hoist the check of the argument.
    for (let i = 0; i < count && !numberOfDFGCompiles(copy); ++i)
        shouldBe(copy(ordinary[i % 16]), 4);
    shouldBe(numberOfDFGCompiles(copy), 1);

    for (let i = 0; i < count; ++i) {
        let index = (i >> 1) % 16;
        shouldBe(copy((i % 2) ? ordinary[index] : arrayStorage[index]), 4);
    }

    // numberOfDFGCompiles() is the reoptimization retry count plus one.
    let compiles = numberOfDFGCompiles(copy);
    if (recordsSamples && compiles > expectedCompiles)
        throw new Error(`${func.name} was compiled ${compiles} times, expected ${expectedCompiles}`);
    if (!recordsSamples && compiles <= expectedCompiles)
        throw new Error(`${func.name} was compiled ${compiles} times without the option, expected more than ${expectedCompiles}`);

    for (let array of unwrapped)
        shouldBe($vm.indexingMode(array), ordinaryMode);
}

test(read, create, createFrozen, 2);
test(store, create, createFrozen, 2);
test(has, create, createFrozen, 2);
test(readAndLength, create, createFrozen, 2);
test(readPropertyWithInlinedHelper, create, createFrozen, 2, (array) => ({ array }));
test(read, createMatrix, createFrozenMatrix, 2);

// The structure check of the load of Symbol.iterator fails first.
test(destructure, create, createFrozen, 3);
test(read, create, createSparse, 3);
test(store, create, createSparse, 3);
test(has, create, createSparse, 3);
test(read, create, createQueue, 3);
test(pushAndPop, create, createQueue, 3);
