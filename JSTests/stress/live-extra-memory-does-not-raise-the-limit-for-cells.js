//@ runDefault("--useConcurrentGC=0")

// The heap collects when the bytes allocated since the last collection pass a limit, and that limit grows from
// everything that was alive then: cells and the extra memory they keep, such as the contents of an ArrayBuffer. One large
// buffer that stays alive raised the limit by its own size or more, and a program could then allocate that many bytes
// of cells, all of them dead, before a collection ran. Cells have a limit of their own. It grows from the cells plus
// the extra memory, where the extra memory counts for no more than the cells do.

function shouldBeTrue(condition, message) {
    if (!condition)
        throw new Error(message);
}

const MB = 1024 * 1024;
const bufferSize = 512 * MB;
const cellBytes = 64 * MB;

function allocateCells(bytes, keep) {
    const start = $vm.heapTotalBytesAllocated();
    while ($vm.heapTotalBytesAllocated() - start < bytes) {
        for (let i = 0; i < 4096; i++) {
            const object = { a: i, b: [i, i + 1], c: { d: i } };
            if (keep)
                keep.push(object);
        }
    }
}
noInline(allocateCells);

const keptAlive = [];
function allocateOldBuffer() {
    keptAlive.push(new ArrayBuffer(MB));
}
noInline(allocateOldBuffer);

function test() {
    // calloc() does not touch the pages, so the buffer costs address space and no memory.
    const buffer = new ArrayBuffer(bufferSize);
    fullGC();
    shouldBeTrue($vm.heapExtraMemorySize() >= bufferSize, "the heap does not count the buffer");

    // What the concurrent collector paces a cycle against is the smaller of the two limits. With the larger one it
    // would let the mutator run on for a share of the size of the buffer while it marks.
    const budget = $vm.heapAllocationBudgetThisCycle();
    shouldBeTrue(budget < cellBytes, `the cycle after a full collection is paced against ${budget} bytes next to a live buffer of ${bufferSize / MB} MB`);

    // Dead cells: a collection has to run long before they add up to the size of the buffer.
    const before = $vm.totalGCTime();
    allocateCells(cellBytes, null);
    shouldBeTrue($vm.totalGCTime() > before, `no collection ran while ${cellBytes / MB} MB of cells were allocated next to a live buffer of ${bufferSize / MB} MB`);

    // Cells that die old: an eden collection promotes them, and only a full collection can free them. One has to run
    // long before the promoted cells add up to the size of the buffer. An old buffer that is dead shows when it did.
    allocateOldBuffer();
    edenGC();
    keptAlive.length = 0;
    const extraMemoryBefore = $vm.heapExtraMemorySize();
    allocateCells(2 * cellBytes, []);
    const freed = extraMemoryBefore - $vm.heapExtraMemorySize();
    // Other extra memory can grow by a few KB in the same time.
    shouldBeTrue(freed >= MB / 2, `no full collection ran while ${2 * cellBytes / MB} MB of cells were kept next to a live buffer of ${bufferSize / MB} MB`);

    shouldBeTrue(buffer.byteLength === bufferSize, "the buffer is alive");
}

// Without the JIT the VM is in mini mode, where every collection is a full one.
if ($vm.useJIT())
    test();
