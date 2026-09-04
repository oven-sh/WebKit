//@ requireOptions("--useJSThreads=1", "--useThreadGIL=0", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1", "--collectContinuously=1", "--useGenerationalGC=0")
// defineOwnIndexedProperty adds to the sparse map under the object's cell lock.
// The add reports the map's new capacity to the heap, and the report can start
// a collection. With the GIL off, this thread conducts that collection and
// waits for its markers, and a marker that visits the object waits for the
// cell lock, so the process stopped. The collection is deferred until the
// lock is released now. Nothing here spawns a thread.
//
// A map reports each time its capacity grows, so many small maps report often.
// Continuous collection keeps a collection pending at most reports.

const OBJECTS = 4000;
const DEFINES = 64;

// A Debug build collects much more slowly, so it stops at a deadline.
const deadline = Date.now() + 20000;
for (let n = 0; n < OBJECTS && Date.now() < deadline; ++n) {
    const object = [];
    for (let i = 0; i < DEFINES; ++i) {
        // A read-only index keeps the object in sparse indexing mode.
        Object.defineProperty(object, i * 3, { value: n + i, writable: false, enumerable: true, configurable: true });
    }
    if (object[3 * (DEFINES - 1)] !== n + DEFINES - 1)
        throw new Error("object " + n + ": the last define was lost");
    if (Object.getOwnPropertyDescriptor(object, 3).writable)
        throw new Error("object " + n + ": index 3 is writable");
}
