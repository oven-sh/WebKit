//@ runDefault("--useConcurrentGC=0", "--minEdenToOldGenerationRatio=2")

// Heap::collectAsyncIfAllocated(minimumBytes) is collectAsync() for an embedder's hint, such as a timer that asks for a
// collection every second in case the program has stopped allocating. The heap queues the collection only if it has
// something to collect: `minimumBytes` allocated since the last collection ended, or the Full collection that an Eden
// collection left owing. It returns whether a collection is queued or under way.

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

var MB = 1024 * 1024;

// 1. Nothing was allocated since a collection ended: no collection, however often the embedder asks.
fullGC();
shouldBe(collectAsyncIfAllocated(MB), false, "a hint with nothing allocated");
shouldBe(collectAsyncIfAllocated(MB), false, "a second hint with nothing allocated");

// 2. Less than the minimum was allocated: no collection. More: one collection, and a hint while it is queued adds none.
var kept = allocate(2);
shouldBe(collectAsyncIfAllocated(64 * MB), false, "a hint with less than the minimum allocated");
shouldBe(collectAsyncIfAllocated(MB), true, "a hint with more than the minimum allocated");
shouldBe(collectAsyncIfAllocated(64 * MB), true, "a hint while a collection is queued");

// 3. Once a collection has run, what was allocated before it does not count.
fullGC();
shouldBe(collectAsyncIfAllocated(MB), false, "a hint after the collection");

// 4. With a ratio that no heap meets (the option above), every Eden collection leaves a Full one owing. Nothing but
//    the next collection runs it, so the hint queues one with nothing allocated. After it, nothing is owed.
edenGC();
shouldBe(collectAsyncIfAllocated(1024 * MB), true, "a hint after an Eden collection that left a Full one owing");
fullGC();
shouldBe(collectAsyncIfAllocated(1024 * MB), false, "a hint after the Full collection");
