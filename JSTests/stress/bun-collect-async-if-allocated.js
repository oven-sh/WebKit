//@ runDefault("--useConcurrentGC=0", "--minEdenToOldGenerationRatio=2")
//@ runDefault("--minEdenToOldGenerationRatio=2")
//@ runDefault("--useConcurrentGC=0", "--criticalGCMemoryThreshold=0.000001")

// Heap::collectAsyncIfAllocated() is collectAsync() for an embedder's hint, such as a timer that asks for a collection
// every second in case the program has stopped allocating. The heap asks for the collection only if it has something to
// do: a cell was allocated since the last collection ended, or a Full collection is owed. It returns whether it asked.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + expected + " but got " + actual);
}

// Objects of 32 bytes that stay alive.
function allocate(megabytes) {
    var kept = [];
    for (var i = 0; i < megabytes * 32768; i++)
        kept.push({ a: i, b: i });
    return kept;
}

// An array whose butterfly is in a block.
var small = [];
for (var i = 0; i < 100; i++)
    small.push(i);

// The answers come first and the checks after them: a call of shouldBe() can allocate (its code block, a tier-up).

// 1. With nothing allocated since the last collection, the heap turns the hint down.
fullGC();
var afterCollection = collectAsyncIfAllocated();
var afterCollectionAgain = collectAsyncIfAllocated();

// 2. One cell is enough for a yes, also one that no byte count has yet: a cell is counted when its allocator has used up
//    a free list. One collection takes the answer back to no.
var one = { a: 1 };
var afterOneObject = collectAsyncIfAllocated();
fullGC();
var afterItsCollections = collectAsyncIfAllocated();

// 3. So is a cell that no block holds. The butterfly of the array becomes a precise allocation, and nothing else is
//    allocated.
small.length = 50000;
var afterPreciseAllocation = collectAsyncIfAllocated();
fullGC();
var afterItsCollection = collectAsyncIfAllocated();

// 4. A Full collection that is owed runs with nothing allocated since the last collection. In the first two runs the
//    option makes every Eden collection leave too little room, so the heap makes the next collection a Full one. In the
//    third run memory use is over the critical threshold, where every collection that has no scope is a Full one (the
//    heap reads the memory use of the process on Linux and macOS). A Full collection leaves none owing.
var kept = allocate(2);
edenGC();
var afterEdenCollection = collectAsyncIfAllocated();
fullGC();
var afterFullCollection = collectAsyncIfAllocated();

shouldBe(afterCollection, false, "a hint after a collection");
shouldBe(afterCollectionAgain, false, "a second hint after a collection");
shouldBe(afterOneObject, true, "a hint after one object");
shouldBe(afterItsCollections, false, "a hint after the collections of that object");
shouldBe(afterPreciseAllocation, true, "a hint after a precise allocation");
shouldBe(afterItsCollection, false, "a hint after the collection of the precise allocation");
shouldBe(afterEdenCollection, true, "a hint after an Eden collection that left a Full one owing");
shouldBe(afterFullCollection, false, "a hint after the Full collection");
