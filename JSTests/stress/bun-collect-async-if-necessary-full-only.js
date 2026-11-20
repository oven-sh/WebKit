//@ runDefault("--useConcurrentGC=0", "--useGenerationalGC=0")
//@ runDefault("--useConcurrentGC=0", "--forceMiniVMMode=1")
//@ runDefault("--useConcurrentGC=0", "--criticalGCMemoryThreshold=0.000001") if $hostOS == "linux" or $hostOS == "darwin"

// Heap::collectAsyncIfNecessary() where a request without a scope runs as a Full collection: a heap without a
// generational collector, a mini VM, and a heap whose process is over the critical memory threshold (the heap reads the
// memory use of the process on Linux and macOS). A Full collection can free what was allocated before the last
// collection, so the heap does not turn the hint down, also with nothing allocated.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + expected + " but got " + actual);
}

// The answers come first and the checks after them: a call of shouldBe() can allocate (its code block, a tier-up).
fullGC();
var afterFullCollection = collectAsyncIfNecessary();
fullGC();
edenGC();
var afterEdenRequest = collectAsyncIfNecessary();

shouldBe(afterFullCollection, true, "a hint after a Full collection");
shouldBe(afterEdenRequest, true, "a hint after a collection that was asked for as an Eden one");
