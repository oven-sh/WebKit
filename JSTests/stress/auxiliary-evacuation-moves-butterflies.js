//@ runDefault("--useConcurrentGC=0")
//@ runDefault("--useConcurrentGC=0", "--useJIT=0")

// Heap::evacuateSparseAuxiliaryBlocks moves the butterflies (out-of-line properties and indexed storage) of objects out of
// sparse blocks. The objects must not notice: same properties, same elements, same array kinds, and they keep working
// (growing, shrinking, transitioning) afterwards; the emptied blocks go away with the next full collection.

function assert(condition, message) {
    if (!condition)
        throw new Error("assertion failed: " + message);
}

function makeObject(i) {
    const object = { a: i, b: i + 1 };
    // Out-of-line properties.
    for (let k = 0; k < 8 + (i % 5); ++k)
        object["p" + k] = i * 100 + k;
    return object;
}
function makeArray(i) {
    switch (i % 6) {
    case 0: return [i, i + 1, i + 2]; // int32
    case 1: return [i + 0.5, i + 1.5]; // double
    case 2: return ["s" + i, { i }, i]; // contiguous
    case 3: { const array = [i, i + 1]; array.named = "n" + i; array.other = i; array.third = -i; array.fourth = [i]; array.fifth = i; array.sixth = i; array.seventh = i; return array; } // elements and out-of-line properties
    case 4: { const array = [i]; array[5] = i + 5; return array; } // holes
    default: { const array = [i, i + 1, i + 2, i + 3]; array[2000 + (i % 50)] = "far" + i; return array; } // ArrayStorage with a sparse map
    }
}
function checkObject(object, i) {
    assert(object.a === i && object.b === i + 1, "inline properties of " + i);
    for (let k = 0; k < 8 + (i % 5); ++k)
        assert(object["p" + k] === i * 100 + k, "out-of-line property p" + k + " of " + i);
}
function checkArray(array, i) {
    switch (i % 6) {
    case 0: assert(array.length === 3 && array[0] === i && array[2] === i + 2, "int32 array " + i); break;
    case 1: assert(array.length === 2 && array[0] === i + 0.5 && array[1] === i + 1.5, "double array " + i); break;
    case 2: assert(array.length === 3 && array[0] === "s" + i && array[1].i === i && array[2] === i, "contiguous array " + i); break;
    case 3: assert(array.length === 2 && array[1] === i + 1 && array.named === "n" + i && array.third === -i && array.fourth[0] === i && array.seventh === i, "array with properties " + i); break;
    case 4: assert(array.length === 6 && array[0] === i && array[5] === i + 5 && !(3 in array), "array with holes " + i); break;
    default: assert(array[3] === i + 3 && array[2000 + (i % 50)] === "far" + i && array.length === 2001 + (i % 50), "sparse array " + i); break;
    }
}

const keptObjects = [];
const keptArrays = [];
function populate() {
    const all = [];
    for (let i = 0; i < 60000; ++i) {
        all.push(makeObject(i));
        all.push(makeArray(i));
    }
    // One in 37 survives: every block of butterflies ends up sparse.
    for (let i = 0; i < 60000; i += 37) {
        keptObjects.push([all[2 * i], i]);
        keptArrays.push([all[2 * i + 1], i]);
    }
    // A stale reference to the array on the stack must not keep everything alive.
    all.length = 0;
}
noInline(populate);
populate();
gc();

function checkAll() {
    for (const [object, i] of keptObjects)
        checkObject(object, i);
    for (const [array, i] of keptArrays)
        checkArray(array, i);
}
noInline(checkAll);
checkAll();

const blocksBefore = $vm.markedBlockStatistics().blocks;
const result = $vm.evacuateAuxiliaryBlocks(0.5);
assert(result.candidateBlocks > 20, "sparse blocks were found: " + JSON.stringify(result));
assert(result.movedCells > 1000, "butterflies were moved: " + JSON.stringify(result));
assert(result.evacuatedBlocks > 20, "blocks were evacuated: " + JSON.stringify(result));
checkAll();

// The moved storage is reachable only through objects that were already marked: the barrier must have remembered them.
edenGC();
checkAll();
gc();
checkAll();
const blocksAfter = $vm.markedBlockStatistics().blocks;
assert(blocksAfter < blocksBefore - result.evacuatedBlocks / 2, "the evacuated blocks were freed: " + blocksBefore + " -> " + blocksAfter + " " + JSON.stringify(result));

// The objects keep working: grow, transition, shrink.
for (const [object, i] of keptObjects) {
    object.later = i;
    delete object.p0;
}
for (const [array, i] of keptArrays) {
    array.push("pushed" + i);
    if (i % 6 === 0)
        array[1] = 1.5; // int32 -> double
}
gc();
for (const [object, i] of keptObjects)
    assert(object.later === i && object.p0 === undefined && object.p1 === i * 100 + 1, "object " + i + " after changes");
for (const [array, i] of keptArrays)
    assert(array[array.length - 1] === "pushed" + i, "array " + i + " after push");

// Again, now that everything is dense: nothing to do, nothing breaks.
const again = $vm.evacuateAuxiliaryBlocks(0.1);
gc();
for (const [array, i] of keptArrays)
    assert(array[array.length - 1] === "pushed" + i, "array " + i + " after the second evacuation");
