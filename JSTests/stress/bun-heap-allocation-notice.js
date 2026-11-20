//@ runDefault("--useConcurrentGC=0")

// Heap::setAllocationNotice(bytes) is a one-shot for an embedder that stops looking at a heap that has gone quiet:
// HeapObserver::didReachAllocationNotice() runs once, when the mutator has allocated `bytes` more than it had at the
// call. allocationNoticesReached() is how many times it ran.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error(message + ": expected " + expected + " but got " + actual);
}

var MB = 1024 * 1024;

// `megabytes` of objects of 32 bytes, and the array that keeps them: a quarter as much again.
function allocate(megabytes) {
    var kept = new Array(megabytes * 32768);
    for (var i = 0; i < kept.length; i++)
        kept[i] = { a: i, b: i };
    return kept;
}

fullGC();
shouldBe(allocationNoticesReached(), 0, "before a notice was asked for");

// 1. One notice, when the mutator is past what was named, and none after it.
setAllocationNotice(4 * MB);
var a = allocate(1);
shouldBe(allocationNoticesReached(), 0, "after 1.25 MB of 4 MB");
var b = allocate(8);
shouldBe(allocationNoticesReached(), 1, "after 11 MB of 4 MB");
var c = allocate(8);
shouldBe(allocationNoticesReached(), 1, "after 10 MB more");

// 2. A second call replaces the first, and 0 cancels.
setAllocationNotice(MB);
setAllocationNotice(64 * MB);
var d = allocate(4);
shouldBe(allocationNoticesReached(), 1, "after 5 MB of 64 MB");
setAllocationNotice(0);
var e = allocate(4);
shouldBe(allocationNoticesReached(), 1, "after a cancelled notice");

// 3. What a collection frees in between is not taken off: the notice is for what the program does, not for what is left.
setAllocationNotice(8 * MB);
allocate(4);
shouldBe(allocationNoticesReached(), 1, "after 5 MB of 8 MB");
fullGC();
shouldBe(allocationNoticesReached(), 1, "after a collection");
allocate(4);
shouldBe(allocationNoticesReached(), 2, "after 10 MB of 8 MB with a collection in between");

// 4. Memory that is not in the heap's blocks counts like the cells do.
var kinds = {
    "an ArrayBuffer": () => new ArrayBuffer(8 * MB),
    "a typed array": () => new Uint8Array(8 * MB),
    "a string": () => "x".repeat(8 * MB),
    "an array of a million numbers": () => {
        var numbers = [];
        for (var i = 0; i < MB; i++)
            numbers.push(i);
        return numbers;
    },
};
var reached = 2;
for (var kind in kinds) {
    setAllocationNotice(4 * MB);
    var kept = kinds[kind]();
    shouldBe(allocationNoticesReached(), ++reached, "after " + kind + " of 8 MB");
}
