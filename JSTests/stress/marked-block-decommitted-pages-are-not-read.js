//@ skip if $hostOS == "windows"
//@ runDefault("--useConcurrentGC=0", "--poisonDecommittedMarkedBlockPages=1")
//@ runDefault("--poisonDecommittedMarkedBlockPages=1", "--collectContinuously=1")
//@ runDefault("--poisonDecommittedMarkedBlockPages=1", "--scribbleFreeCells=1")

// Pages of a MarkedBlock that hold no live cell are given back to the OS after a full collection's sweep. Nothing may
// read them until the block is swept to a free list again: not the next sweeps of a block whose cells have destructors,
// not heap iteration, not the allocator. poisonDecommittedMarkedBlockPages turns such a read into a crash.
// Skipped on Windows, where MarkedBlock::Handle::decommitUnusedPages() is compiled out.

function assert(condition, message) {
    if (!condition)
        throw new Error("assertion failed: " + message);
}

const keep = [];
function churn(round) {
    const all = [];
    for (let i = 0; i < 30000; ++i) {
        // Cells with and without destructors, butterflies, ropes and resolved strings.
        const object = { a: i, s: "s" + i + round, r: ("r" + i) + ("t" + round), array: [i, i + 1, i + 2, i + 3], date: new Date(i), map: i % 64 ? null : new Map([[i, i]]) };
        object["extra" + (i % 5)] = i;
        all.push(object);
    }
    for (let i = round; i < all.length; i += 331)
        keep.push(all[i]);
}
noInline(churn);

for (let round = 0; round < 4; ++round) {
    churn(round);
    gc();
    gc();
    // Walks every live cell.
    generateHeapSnapshot();
    edenGC();
}
const stats = $vm.markedBlockStatistics();
if (stats.pagesPerBlock > 1) {
    assert(stats.decommittedPages > 0, "some pages were decommitted: " + JSON.stringify(stats));
    assert($vm.decommittedMarkedBlockPagePoison() !== "none", "decommitted pages are poisoned");
}
for (const object of keep) {
    assert(object.s.startsWith("s"), "string survived");
    assert(object.r.length >= 4, "rope survived");
    assert(object.array.length === 4, "array survived");
}
// Drop most of what was kept so that blocks with decommitted pages empty out and are freed.
keep.length = 10;
gc();
churn(9);
gc();
