//@ requireOptions("--useJSThreads=1", "--useThreadGIL=0", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1", "--verifyConcurrentButterfly=1")
// More paths that read an array's storage through butterfly(), which must not
// decode a segmented word: the fast clone of Array.from, concat and toSorted,
// the indexed length of a for-in, and the heap snapshot. Each one reads
// through the word now, and a segmented array takes the generic path. With
// --verifyConcurrentButterfly, a decode of a segmented word fails an assertion.

const ROUNDS = 10;
const SIZE = 16;

// A write from another thread, then a store past the end: the storage grows
// while it is shared-written, so it is segmented from then on. The store past
// the end leaves holes, so the length is 41.
function segmentedArray() {
    const array = [];
    for (let i = 0; i < SIZE; ++i)
        array.push("v" + i);
    new Thread(() => {
        array[0] = "w";
        array[40] = "far";
    }).join();
    return array;
}

function check(what, copy) {
    if (copy.length !== 41 || copy[0] !== "w" || copy[1] !== "v1" || copy[40] !== "far")
        throw new Error(what + " returned " + JSON.stringify(copy));
}

for (let round = 0; round < ROUNDS; ++round) {
    check("Array.from", Array.from(segmentedArray()));
    check("concat", segmentedArray().concat());

    const sorted = segmentedArray().toSorted();
    if (sorted.length !== 41 || sorted[0] !== "far" || sorted[1] !== "v1")
        throw new Error("toSorted returned " + JSON.stringify(sorted));

    const keys = [];
    for (const key in segmentedArray())
        keys.push(key);
    if (keys.length !== 17 || keys[1] !== "1" || keys[16] !== "40")
        throw new Error("for-in saw " + JSON.stringify(keys));

    // The snapshot walks every object, this array included.
    const array = segmentedArray();
    const snapshot = generateHeapSnapshot();
    if (!snapshot.nodes || !snapshot.nodes.length)
        throw new Error("the heap snapshot is empty");
    check("the array after the snapshot", array);
}
