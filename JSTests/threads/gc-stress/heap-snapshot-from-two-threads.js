//@ requireOptions("--useJSThreads=1")
// Two threads take heap snapshots (which collect synchronously under a
// "prevent collection" scope) at the same time, while a third allocates. With
// the GIL off the second thread must wait for the first thread's scope in a
// way that lets the first thread's collection stop it; otherwise neither
// finishes.
load("../harness.js", "caller relative");

if (typeof generateHeapSnapshotForGCDebugging !== "function")
    throw new Error("needs the jsc shell's generateHeapSnapshotForGCDebugging");

const ROUNDS = 6;

// Bounded, so the test also ends under the cooperative GIL.
const allocator = new Thread(() => {
    let junk = [];
    for (let i = 0; i < 400000; ++i) {
        junk.push({ a: junk.length, s: "x" + junk.length });
        if (junk.length > 5000)
            junk = [];
    }
    return true;
});

function takeSnapshot() {
    // The shell hands back the snapshot's JSON text.
    const snapshot = generateHeapSnapshotForGCDebugging();
    return typeof snapshot === "string" && snapshot.length > 2 && snapshot.indexOf("nodes") >= 0;
}

const snappers = spawnN(2, (t) => {
    let good = 0;
    for (let i = 0; i < ROUNDS; ++i)
        good += takeSnapshot() ? 1 : 0;
    return good;
});
// The main thread joins in as well.
let mainSnapshots = 0;
for (let i = 0; i < ROUNDS; ++i)
    mainSnapshots += takeSnapshot() ? 1 : 0;

const results = joinAll(snappers);
shouldBe(allocator.join(), true, "allocator finished");
shouldBe(mainSnapshots, ROUNDS, "main thread snapshots");
for (let t = 0; t < 2; ++t)
    shouldBe(results[t], ROUNDS, "snapshots on thread " + t);
