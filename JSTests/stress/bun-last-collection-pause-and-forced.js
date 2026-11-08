//@ runDefault

// What the heap keeps about the last collection that ended, for an embedder that reports collections: whether its
// request was tagged as forced (forcedFullGC() here, GCRequest::isForced), and for how long it had the mutator stopped.
// The shell waits for a collection that it asks for, so the stops of that collection are inside the call.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": expected " + expected + " but got " + actual);
}

function check(collect, name, forced) {
    var before = preciseTime();
    collect();
    var elapsed = (preciseTime() - before) * 1000;
    var pause = lastCollectionPause();
    shouldBe(lastCollectionWasForced(), forced, name + " is forced");
    if (!(pause > 0))
        throw new Error(name + ": the pause is " + pause + " ms");
    if (!(pause <= elapsed))
        throw new Error(name + ": the pause of " + pause + " ms is longer than the call, " + elapsed + " ms");
}

for (var round = 0; round < 3; round++) {
    check(fullGC, "fullGC()", false);
    check(forcedFullGC, "forcedFullGC()", true);
    check(edenGC, "edenGC()", false);
    check(forcedFullGC, "forcedFullGC()", true);
    check(idleFullGC, "idleFullGC()", false);
}
