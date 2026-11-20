//@ runDefault("--useConcurrentGC=0", "--minEdenToOldGenerationRatio=2")
//@ runDefault("--minEdenToOldGenerationRatio=2")

// Heap::collectAsyncIfNecessary() is collectAsync() for an embedder's hint, such as a timer that asks for a collection
// every second in case the program has stopped allocating. The heap turns the hint down, and returns false, only if the
// collection would be an Eden one with nothing to sweep.

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

// A rope of 80,000 characters.
var rope = "a".repeat(40000) + "b".repeat(40000);

// The answers come first and the checks after them: a call of shouldBe() can allocate (its code block, a tier-up).

// 1. With nothing allocated since the last collection, the heap turns the hint down.
fullGC();
var afterCollection = collectAsyncIfNecessary();
var afterCollectionAgain = collectAsyncIfNecessary();

// 2. One cell is enough for a yes, also one that no byte count has yet: a cell is counted when its allocator has used up
//    a free list. One collection takes the answer back to no.
var one = { a: 1 };
var afterOneObject = collectAsyncIfNecessary();
fullGC();
var afterItsCollections = collectAsyncIfNecessary();

// 3. So is a cell that is in no block and that the heap counts no bytes for: one of the first cells of a type with its
//    own subspace (a lower-tier precise cell). gc() also sweeps, and it frees the blocks that are empty, so the cell can
//    take none of them.
gc();
var afterSweep = collectAsyncIfNecessary();
var weakSet = new WeakSet;
var afterLowerTierCell = collectAsyncIfNecessary();
fullGC();
var afterItsCollection = collectAsyncIfNecessary();

// 4. So is memory that the heap counts without a cell. The rope gets its characters, which it reports as extra memory.
rope.charCodeAt(0);
var afterExtraMemory = collectAsyncIfNecessary();
fullGC();
var afterExtraMemoryCollection = collectAsyncIfNecessary();

// 5. A request that would run as a Full collection is not turned down. The option makes every Eden collection leave too
//    little room, so the heap makes the next collection a Full one, with nothing allocated since. A Full collection
//    leaves none owing.
var kept = allocate(2);
edenGC();
var afterEdenCollection = collectAsyncIfNecessary();
fullGC();
var afterFullCollection = collectAsyncIfNecessary();

shouldBe(afterCollection, false, "a hint after a collection");
shouldBe(afterCollectionAgain, false, "a second hint after a collection");
shouldBe(afterOneObject, true, "a hint after one object");
shouldBe(afterItsCollections, false, "a hint after the collections of that object");
shouldBe(afterSweep, false, "a hint after a collection that also swept");
shouldBe(afterLowerTierCell, true, "a hint after a lower-tier precise cell");
shouldBe(afterItsCollection, false, "a hint after the collection of the lower-tier precise cell");
shouldBe(afterExtraMemory, true, "a hint after extra memory was reported");
shouldBe(afterExtraMemoryCollection, false, "a hint after the collection of the extra memory");
shouldBe(afterEdenCollection, true, "a hint after an Eden collection that left a Full one owing");
shouldBe(afterFullCollection, false, "a hint after the Full collection");
