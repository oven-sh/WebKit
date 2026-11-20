//@ runDefault("--useConcurrentGC=0", "--useGenerationalGC=0")
//@ runDefault("--useConcurrentGC=0", "--forceMiniVMMode=1")

// Heap::collectAsyncIfAllocated() in a heap where every collection is a Full one: one without a generational collector,
// and a mini VM. The rules are those of bun-collect-async-if-allocated.js. Such a heap owes no collection for being what
// it is.

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
fullGC();
var afterCollection = collectAsyncIfAllocated();

var one = { a: 1 };
fullGC();
var afterCollectionOfOneObject = collectAsyncIfAllocated();

var two = { a: 2 };
var afterOneObject = collectAsyncIfAllocated();
fullGC();
var afterItsCollections = collectAsyncIfAllocated();

// Asked for as an Eden collection, it runs as a Full one and leaves none owing.
var kept = allocate(2);
edenGC();
var afterEdenRequest = collectAsyncIfAllocated();

shouldBe(afterCollection, false, "a hint after a collection");
shouldBe(afterCollectionOfOneObject, false, "a hint after the collection of one object");
shouldBe(afterOneObject, true, "a hint after one object");
shouldBe(afterItsCollections, false, "a hint after the collections of that object");
shouldBe(afterEdenRequest, false, "a hint after a collection that was asked for as an Eden one");
