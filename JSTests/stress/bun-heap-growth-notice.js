//@ runDefault("--useConcurrentGC=0")

// Heap::notifyObserversAfterGrowth(bytes) is a one-shot for an embedder that stops looking at a heap that has gone
// quiet: HeapObserver::didGrow() runs once, when the blocks the heap has allocated and the extra memory its cells hold
// on to are `bytes` more than they were. heapGrowthNotices() is how many times it ran.

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

fullGC();
shouldBe(heapGrowthNotices(), 0, "before a notice was asked for");

// 1. One notice, when the heap is past what was named, and none after it.
notifyAfterHeapGrowth(4 * MB);
var a = allocate(1);
shouldBe(heapGrowthNotices(), 0, "after 1 MB of 4 MB");
var b = allocate(8);
shouldBe(heapGrowthNotices(), 1, "after 9 MB of 4 MB");
var c = allocate(8);
shouldBe(heapGrowthNotices(), 1, "after 8 MB more");

// 2. A second call replaces the first, and 0 cancels.
notifyAfterHeapGrowth(MB);
notifyAfterHeapGrowth(64 * MB);
var d = allocate(4);
shouldBe(heapGrowthNotices(), 1, "after 4 MB of 64 MB");
notifyAfterHeapGrowth(0);
var e = allocate(4);
shouldBe(heapGrowthNotices(), 1, "after a cancelled notice");

// 3. The memory of an ArrayBuffer counts like the blocks do.
notifyAfterHeapGrowth(4 * MB);
var buffer = new ArrayBuffer(8 * MB);
shouldBe(heapGrowthNotices(), 2, "after an ArrayBuffer of 8 MB");
