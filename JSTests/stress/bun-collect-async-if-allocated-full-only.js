//@ runDefault("--useConcurrentGC=0", "--useGenerationalGC=0")
//@ runDefault("--useConcurrentGC=0", "--forceMiniVMMode=1")

// Heap::collectAsyncIfAllocated(minimumBytes) in a heap where every collection is a Full one: one without a generational
// collector, and a mini VM. The rules are those of bun-collect-async-if-allocated.js. Such a heap owes no collection
// for being what it is.

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

shouldBe(collectAsyncIfAllocated(1024 * MB), false, "a hint before the first collection");

var kept = allocate(2);
shouldBe(collectAsyncIfAllocated(64 * MB), false, "a hint with less than the minimum allocated");
shouldBe(collectAsyncIfAllocated(MB), true, "a hint with more than the minimum allocated");

fullGC();
shouldBe(collectAsyncIfAllocated(MB), false, "a hint after the collection");

// Asked for as an Eden collection, it runs as a Full one and leaves none owing.
edenGC();
shouldBe(collectAsyncIfAllocated(1024 * MB), false, "a hint after a collection that was asked for as an Eden one");

// A hint for anything: one collection takes the answer back to no.
var one = { a: 1 };
fullGC();
shouldBe(collectAsyncIfAllocated(0), false, "a hint for anything after the collection of one object");
var two = { a: 2 };
shouldBe(collectAsyncIfAllocated(0), true, "a hint for anything after another object");
