// Heap::collectAsyncIfAllocated(minimumBytes) while a collection that does not cover the hint is under way: the hint
// adds none, because what was allocated is that collection's. The second argument of the shell function requests an
// Eden collection first. A hint has no scope, so it could be a Full collection, and an Eden request does not stand in
// for it. With the concurrent collector, 64 MB of young objects keep that Eden collection marking while the hint asks.

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
var kept = allocate(64);
shouldBe(collectAsyncIfAllocated(1024 * MB, true), true, "a hint while an Eden collection is under way");
fullGC();
shouldBe(collectAsyncIfAllocated(1024 * MB), false, "a hint after the collections");
