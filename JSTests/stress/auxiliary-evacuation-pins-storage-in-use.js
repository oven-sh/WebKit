//@ runDefault("--useConcurrentGC=0")
//@ runDefault("--useConcurrentGC=0", "--useBaselineJIT=0")

// Heap::evacuateSparseAuxiliaryBlocks leaves storage in place that the stack still refers to: storage that a word of the
// stack or a register points into (including the butterfly pointer of an object without indexed storage, which lies
// sizeof(IndexingHeader) past the end of its allocation), and the storage of every object that the stack refers to,
// whether that object is a cell of a MarkedBlock or a precise allocation. Everything else in a sparse block moves.

// These come first: the first few cells of a kind are precise allocations, not cells of a MarkedBlock.
const early = [];
for (let i = 0; i < 12; ++i)
    early.push([]);

function assert(condition, message) {
    if (!condition)
        throw new Error("assertion failed: " + message);
}
function storageOf(object) { return /butterfly (0x[0-9a-f]+)/.exec(describe(object))[1]; }
function addressOf(object) { return parseInt(/Object: (0x[0-9a-f]+)/.exec(describe(object))[1]); }

function makeArray(seed) { return [seed, seed + 1, seed + 2, seed + 3, seed + 4, seed + 5, seed + 6, seed + 7, seed + 8, seed + 9]; }
function makeGrownArray(seed) { const array = []; for (let j = 0; j < 10; ++j) array.push(seed + j); return array; }
function makeObject(seed) { const object = {}; for (let k = 0; k < 14; ++k) object["p" + k] = seed + k; return object; }
function check(object, seed) {
    if (Array.isArray(object)) {
        for (let i = 0; i < 10; ++i)
            assert(object[i] === seed + i, "element " + i + " of array " + seed);
    } else {
        for (let k = 0; k < 14; ++k)
            assert(object["p" + k] === seed + k, "property p" + k + " of object " + seed);
    }
}
noInline(makeArray); noInline(makeObject); noInline(makeGrownArray);

// Each survivor is followed by more than a block's worth of garbage of the same size, so every survivor sits in a sparse block.
function garbage(make) { for (let j = 0; j < 450; ++j) make(-j); }
noInline(garbage);
function makeSparse(make, count) {
    const result = [];
    for (let i = 0; i < count; ++i) {
        result.push(make(100 * i));
        garbage(make);
    }
    return result;
}
noInline(makeSparse);
function fillEarly() {
    for (let i = 0; i < early.length; ++i) {
        for (let j = 0; j < 10; ++j)
            early[i].push(100 * i + j);
        garbage(makeGrownArray);
    }
}
noInline(fillEarly);
function countMoved(objects, before) {
    let moved = 0;
    for (let i = 0; i < objects.length; ++i) {
        check(objects[i], 100 * i);
        if (storageOf(objects[i]) !== before[i])
            moved++;
    }
    return moved;
}

// 1. The storage of an object that is on the stack (here: an argument of a frame that does not touch its storage) stays.
function evacuateWith(a, b, c, d) {
    gc();
    const result = $vm.evacuateAuxiliaryBlocks(0.5);
    return [result, a, b, c, d];
}
noInline(evacuateWith);
{
    fillEarly();
    const arrays = makeSparse(makeArray, 8), objects = makeSparse(makeObject, 8);
    const precise = early.findIndex((array) => addressOf(array) % 16 === 8);
    const inBlock = early.findIndex((array) => addressOf(array) % 16 === 0);
    assert(precise >= 0 && inBlock >= 0, "the first arrays are precise allocations, later ones are not");
    const all = [...early, ...arrays, ...objects];
    const before = all.map(storageOf);
    const [result] = evacuateWith(early[precise], early[inBlock], arrays[3], objects[3]);
    assert(!result.skipped && result.pinnedCells >= 4, "four were pinned: " + JSON.stringify(result));
    let moved = 0;
    for (let i = 0; i < all.length; ++i) {
        const stayed = storageOf(all[i]) === before[i];
        if (all[i] === early[precise] || all[i] === early[inBlock] || all[i] === arrays[3] || all[i] === objects[3])
            assert(stayed, "the storage of object " + i + ", which is on the stack, stays");
        else if (!stayed)
            moved++;
    }
    assert(moved >= all.length - 8, "what nothing on the stack refers to moves: " + moved + " of " + all.length + " " + JSON.stringify(result));
    assert(countMoved(early, before) + countMoved(arrays, before.slice(12)) + countMoved(objects, before.slice(20)) === moved, "contents");
}

// 2. A native frame keeps a pointer to the storage, and nothing of its owner (see $vm.evacuateAuxiliaryBlocks). Elements
// only: the pointer is inside the allocation. Out-of-line properties only: it is sizeof(IndexingHeader) past the end.
function evacuateHolding(holder) {
    gc();
    return $vm.evacuateAuxiliaryBlocks(0.5, holder);
}
noInline(evacuateHolding);
function makeHolder(object) { return { target: object }; }
noInline(makeHolder);
for (const make of [makeArray, makeObject]) {
    const objects = makeSparse(make, 8);
    const before = objects.map(storageOf);
    const result = evacuateHolding(makeHolder(objects[5]));
    assert(result.heldStorageStayed === true && result.pinnedCells >= 1, "storage that a native frame points to stays: " + JSON.stringify(result));
    assert(countMoved(objects, before) >= 5, "the rest moves: " + JSON.stringify(result));
}
