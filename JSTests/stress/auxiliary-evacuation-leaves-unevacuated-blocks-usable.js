//@ runDefault("--useConcurrentGC=0")

// A sparse block that Heap::evacuateSparseAuxiliaryBlocks cannot empty (here: each holds the property name buffer of a
// for-in enumerator, which is not a butterfly) is taken out of the allocatable set while the others are evacuated, and put
// back afterwards: what is allocated next goes into its free cells instead of into new blocks.

function assert(condition, message) {
    if (!condition)
        throw new Error("assertion failed: " + message);
}
function makeArray(i) { return [i, i + 1, i + 2, i + 3, i + 4, i + 5, i + 6, i + 7, i + 8]; }
noInline(makeArray);
const shapes = [];
function populate() {
    for (let i = 0; i < 150; ++i) {
        const object = {};
        for (let k = 0; k < 10; ++k)
            object["p" + i + "_" + k] = k;
        let names = 0;
        for (const name in object)
            names++;
        assert(names === 10, "for-in");
        shapes.push(object);
        for (let j = 0; j < 450; ++j)
            makeArray(j);
    }
}
noInline(populate);
populate();
gc();
const result = $vm.evacuateAuxiliaryBlocks(0.5);
assert(!result.skipped && result.candidateBlocks >= 100 && result.cellsWithoutSingleOwner >= 100, "most sparse blocks hold a cell that cannot move: " + JSON.stringify(result));
const blocksBefore = $vm.markedBlockStatistics().blocks;
const kept = [];
for (let i = 0; i < 20000; ++i)
    kept.push(makeArray(i));
const blocksAfter = $vm.markedBlockStatistics().blocks;
assert(blocksAfter - blocksBefore < 70, "new storage went into the free cells of the sparse blocks: " + blocksBefore + " -> " + blocksAfter + " blocks " + JSON.stringify(result));
for (let i = 0; i < kept.length; i += 97)
    assert(kept[i][8] === i + 8, "array " + i);
