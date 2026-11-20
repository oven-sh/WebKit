//@ runDefault("--useConcurrentGC=0", "--minEdenToOldGenerationRatio=2")

// Heap::collectAsyncIfAllocated() is collectAsync() for an embedder's hint, such as a timer that asks for a collection
// every second in case the program has stopped allocating. The heap asks for the collection only if it has something to
// collect: a cell that the mutator allocated since the last collection began, or the Full collection that an Eden
// collection left owing. It returns whether it asked.

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

// The answers come first and the checks after them: a call of shouldBe() can allocate (its code block, a tier-up).

// 1. With nothing allocated since the last collection, the heap turns the hint down.
fullGC();
var afterCollection = collectAsyncIfAllocated();
var afterCollectionAgain = collectAsyncIfAllocated();

// 2. One collection is enough for that. It begins with an allocator in the middle of a block, and the mutator takes no
//    cell from the block while the collection runs.
var one = { a: 1 };
fullGC();
var afterCollectionOfOneObject = collectAsyncIfAllocated();

// 3. One cell is enough for a yes, also one that no byte count has yet: a cell is counted when its allocator has used up
//    a free list.
var two = { a: 2 };
var afterOneObject = collectAsyncIfAllocated();
fullGC();
var afterItsCollections = collectAsyncIfAllocated();

// 4. After an Eden collection that leaves little room, the heap makes the next collection a Full one, and nothing but the
//    next request runs it. The option makes every Eden collection such a one. The hint asks for that Full collection
//    with nothing allocated since.
var kept = allocate(2);
edenGC();
var afterEdenCollection = collectAsyncIfAllocated();
fullGC();
var afterFullCollection = collectAsyncIfAllocated();

shouldBe(afterCollection, false, "a hint after a collection");
shouldBe(afterCollectionAgain, false, "a second hint after a collection");
shouldBe(afterCollectionOfOneObject, false, "a hint after the collection of one object");
shouldBe(afterOneObject, true, "a hint after one object");
shouldBe(afterItsCollections, false, "a hint after the collections of that object");
shouldBe(afterEdenCollection, true, "a hint after an Eden collection that left a Full one owing");
shouldBe(afterFullCollection, false, "a hint after the Full collection");
