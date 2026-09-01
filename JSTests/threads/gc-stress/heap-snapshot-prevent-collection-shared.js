//@ requireOptions("--useJSThreads=1", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1")
// heap-snapshot-prevent-collection-shared.js — once the heap is a shared
// server (the GIL-off trio flips it at VM construction), a
// PreventCollectionScope holder must still be able to run its own
// collectNow(Sync): the heap snapshot builders call collectNow(Sync, Full)
// inside the scope, so the shared conduct-tenure gate has to exempt the
// holder thread. Without the exemption the holder's election loops on 1 ms
// follower waits forever and the first snapshot never returns; the runner
// timeout is the failure mode, the assertions below are the success mode.

const HAVE_SNAPSHOT = typeof generateHeapSnapshot === "function";
const HAVE_SNAPSHOT_GCDEBUG = typeof generateHeapSnapshotForGCDebugging === "function";

if (!HAVE_SNAPSHOT && !HAVE_SNAPSHOT_GCDEBUG)
    throw new Error("shell without heap snapshot hooks");

// Some garbage so the collection inside the prevent scope has work to do.
let keep = [];
for (let i = 0; i < 2000; ++i)
    keep.push({ i, s: "x" + (i & 63), a: [i, i + 1] });

for (let round = 0; round < 3; ++round) {
    if (HAVE_SNAPSHOT) {
        const snap = generateHeapSnapshot();
        if (typeof snap === "string") {
            const parsed = JSON.parse(snap);
            if (!parsed || typeof parsed !== "object")
                throw new Error("round " + round + ": snapshot parsed to non-object");
        } else if (!snap || typeof snap !== "object")
            throw new Error("round " + round + ": unexpected snapshot type " + typeof snap);
    }
    if (HAVE_SNAPSHOT_GCDEBUG)
        generateHeapSnapshotForGCDebugging();
    // The gate must be fully lowered again: an ordinary sync collection
    // after the scope closes has to complete too.
    if (typeof gc === "function")
        gc();
}

if (keep.length !== 2000 || keep[1999].i !== 1999)
    throw new Error("retained array corrupted across snapshots");
print("PASS");
