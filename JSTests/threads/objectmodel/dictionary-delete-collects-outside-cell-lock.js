//@ requireOptions("--useJSThreads=1", "--useThreadGIL=0", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1", "--collectContinuously=1", "--useGenerationalGC=0", "--forceDidDeferGCWork=1", "--useDollarVM=1")
// A delete from a dictionary object runs under the object's cell lock, and the
// structure's remove takes a GCSafeConcurrentJSLocker inside it. That locker's
// DeferGC ended under the cell lock, and its release can conduct a pending
// collection. With the GIL off, this thread then waits for the markers, and a
// marker that visits the object waits for the cell lock, so the process
// stopped. The delete defers collection across the whole lock now. Nothing
// here spawns a thread. Continuous collection keeps a collection pending, and
// --forceDidDeferGCWork makes each DeferGC release poll for it.

const KEYS = 200;
const CYCLES = 2000000;

const keys = [];
for (let i = 0; i < KEYS; ++i)
    keys.push("k" + i);
const object = {};
for (let i = 0; i < KEYS; ++i)
    object[keys[i]] = i;
// The locked delete is the one for an uncacheable dictionary. A marker takes
// the cell lock only for an object with ArrayStorage, which a read-only index
// gives it.
Object.defineProperty(object, 0, { value: 0, writable: false, enumerable: true, configurable: true });
$vm.toUncacheableDictionary(object);

// A Debug build collects much more slowly, so it stops at a deadline.
const deadline = Date.now() + 20000;
for (let n = 0; n < CYCLES; ++n) {
    const key = keys[n % KEYS];
    delete object[key];
    if (key in object)
        throw new Error("cycle " + n + ": " + key + " was not deleted");
    object[key] = n;
    if (!(n & 1023) && Date.now() > deadline)
        break;
}
