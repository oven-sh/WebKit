//@ runDefault

// What the heap keeps about the last collection that ended, for an embedder that reports collections: whether its
// request was tagged as forced (forcedFullGC() here, GCRequest::isForced), and for how long it kept the mutator from
// running. The shell waits for a collection that it asks for. So the mutator is kept from running for all of the cycle,
// and not only for the stops of the collection.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": expected " + expected + " but got " + actual);
}

function check(collect, name, forced) {
    // The clock of the pause, in milliseconds.
    var before = $.agent.monotonicNow();
    collect();
    var elapsed = $.agent.monotonicNow() - before;
    var pause = lastCollectionPause();
    shouldBe(lastCollectionWasForced(), forced, name + " is forced");
    shouldBe(pause, lastCollectionLength(), name + ": the pause of a collection that the mutator waited for");
    if (!(pause > 0))
        throw new Error(name + ": the pause is " + pause + " ms");
    if (!(pause <= elapsed))
        throw new Error(name + ": the pause of " + pause + " ms is longer than the call, " + elapsed + " ms");
}

// Live objects, so that a full collection can have more than one stop.
var live = [];
for (var i = 0; i < testLoopCount; i++)
    live.push({ i: i, pair: [i, i] });

check(fullGC, "fullGC()", false);
check(forcedFullGC, "forcedFullGC()", true);
check(edenGC, "edenGC()", false);
check(forcedFullGC, "forcedFullGC()", true);
check(idleFullGC, "idleFullGC()", false);
