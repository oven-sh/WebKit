//@ requireOptions("--useJSThreads=1")
// Heap snapshots taken while other threads allocate. Starting a snapshot
// materializes the SymbolTables that a bytecode cache left pending, which walks
// the heap with a HeapIterationScope; with the GIL off that walk is legal only
// while no other thread can allocate (it stops allocation, and on a shared heap
// that flushed the other threads' free lists out from under them: "finish using
// on a block that's not in use", or a crash in the allocator). The snapshot
// builders now run it with the other threads stopped.
load("../harness.js", "caller relative");

if (typeof generateHeapSnapshot !== "function")
    throw new Error("needs the jsc shell's generateHeapSnapshot");

const THREADS = 4;
const SNAPSHOTS = 12;

let stop = false;
const threads = [];
for (let t = 0; t < THREADS; ++t) {
    threads.push(new Thread(() => {
        let junk = [];
        let made = 0;
        while (!stop) {
            for (let i = 0; i < 2000; ++i)
                junk.push({ a: i, b: [i, i + 1], s: "t" + i });
            made += junk.length;
            junk = [];
        }
        return made;
    }));
}

for (let i = 0; i < SNAPSHOTS; ++i) {
    // The plain snapshot: the GC-debugging one also lists every cell the threads left for the next collection
    // (hundreds of thousands here), which a Debug build prints for seconds with every thread stopped.
    let snapshot = generateHeapSnapshot();
    if (typeof snapshot === "string")
        snapshot = JSON.parse(snapshot);
    if (!snapshot.nodes || !snapshot.nodes.length)
        throw new Error("empty snapshot " + i);
}
stop = true;
// GIL on the threads run only once the main thread blocks in join(), after the snapshots.
for (const thread of threads)
    thread.join();
