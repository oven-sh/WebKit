//@ requireOptions("--useJSThreads=1", "--useThreadGIL=0", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1", "--verifyConcurrentButterfly=1")
// The fast paths of Object.assign, Object.entries and Object.values, and the
// spread of an arguments object, read an object's indexed storage through
// butterfly(), which must not decode a segmented word. Another thread can
// segment the storage, so these paths read the length and the elements through
// the word now, and a segmented arguments object takes the generic spread.
// With --verifyConcurrentButterfly, a decode of a segmented word fails an
// assertion.

const ROUNDS = 20;

// A write from another thread, then a store past the end: the storage grows
// while it is shared-written, so it is segmented from then on.
function segment(object) {
    new Thread(() => {
        object[0] = "w";
        object[40] = "far";
    }).join();
    return object;
}

function segmentedObject() {
    const object = {};
    for (let i = 0; i < 16; ++i)
        object[i] = "v" + i;
    return segment(object);
}

function segmentedArguments() {
    return segment((function () { "use strict"; return arguments; })("a", "b", "c"));
}

for (let round = 0; round < ROUNDS; ++round) {
    const assigned = Object.assign({}, segmentedObject());
    if (assigned[0] !== "w" || assigned[1] !== "v1" || assigned[40] !== "far")
        throw new Error("Object.assign copied " + JSON.stringify(assigned));

    const entries = Object.entries(segmentedObject());
    if (entries.length !== 17 || entries[1][1] !== "v1" || entries[16][1] !== "far")
        throw new Error("Object.entries returned " + JSON.stringify(entries));

    const values = Object.values(segmentedObject());
    if (values.length !== 17 || values[1] !== "v1" || values[16] !== "far")
        throw new Error("Object.values returned " + JSON.stringify(values));

    // The arguments object's length stays 3: an indexed store does not change it.
    const spread = [...segmentedArguments()];
    if (spread.length !== 3 || spread[0] !== "w" || spread[1] !== "b" || spread[2] !== "c")
        throw new Error("the spread of arguments returned " + JSON.stringify(spread));
}
