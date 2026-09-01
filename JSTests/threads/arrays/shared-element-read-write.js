//@ requireOptions("--useJSThreads=1")
// Shared array element reads and writes across threads, covering the major
// indexing types (int32, double, contiguous, and mixed) without resizing.
load("../resources/assert.js", "caller relative");

// --- Foreign-thread reads of every indexing type ---

const int32Array = [1, 2, 3, 4, 5];
const doubleArray = [0.5, 1.5, 2.5, -0.0, NaN];
const contiguousArray = ["a", { name: "obj" }, null, undefined, true];

shouldBe(new Thread(arr => arr[0] + arr[4], int32Array).join(), 6);
shouldBe(new Thread(arr => arr[0] + arr[2], doubleArray).join(), 3);
shouldBe(new Thread(arr => arr[3], doubleArray).join(), -0);
shouldBe(new Thread(arr => arr[4], doubleArray).join(), NaN);
shouldBe(new Thread(arr => arr[1], contiguousArray).join(), contiguousArray[1]);
shouldBe(new Thread(arr => arr[2], contiguousArray).join(), null);
shouldBe(new Thread(arr => arr[3], contiguousArray).join(), undefined);

// Out-of-bounds reads from a foreign thread.
shouldBe(new Thread(arr => arr[100], int32Array).join(), undefined);
shouldBe(new Thread(arr => arr[-1], int32Array).join(), undefined);

// --- Foreign-thread writes, visible to the spawning thread after join ---

const target = [10, 20, 30, 40];
new Thread(arr => { arr[1] = 21; arr[3] = 41; }, target).join();
shouldBe(target[0], 10);
shouldBe(target[1], 21);
shouldBe(target[2], 30);
shouldBe(target[3], 41);
shouldBe(target.length, 4);

// Writes made before spawning are visible inside the thread.
target[0] = 11;
shouldBe(new Thread(arr => arr[0], target).join(), 11);

// --- Foreign-thread writes that change the indexing type ---

// Int32 -> Double.
const toDouble = [1, 2, 3];
new Thread(arr => { arr[1] = 2.5; }, toDouble).join();
shouldBe(toDouble[0], 1);
shouldBe(toDouble[1], 2.5);
shouldBe(toDouble[2], 3);

// Int32 -> Contiguous (boxed).
const toContiguous = [1, 2, 3];
const box = { tag: "boxed" };
new Thread((arr, value) => { arr[2] = value; }, toContiguous, box).join();
shouldBe(toContiguous[2], box);
shouldBe(toContiguous[0], 1);

// Double -> Contiguous.
const doubleToContiguous = [0.5, 1.5];
new Thread(arr => { arr[0] = "str"; }, doubleToContiguous).join();
shouldBe(doubleToContiguous[0], "str");
shouldBe(doubleToContiguous[1], 1.5);

// --- Many threads writing disjoint ranges of one shared array ---

const threadCount = 4;
const perThread = 256;
const slab = new Array(threadCount * perThread).fill(0);
joinAll(spawnN(threadCount, index => {
    const base = index * perThread;
    for (let i = 0; i < perThread; ++i)
        slab[base + i] = base + i + 1;
}));
shouldBe(slab.length, threadCount * perThread);
for (let i = 0; i < slab.length; ++i)
    shouldBe(slab[i], i + 1, "slab[" + i + "]");

// --- Same element hammered by many threads under a lock ---

const lock = new Lock();
const counterArray = [0];
joinAll(spawnN(4, () => {
    for (let i = 0; i < 500; ++i)
        lock.hold(() => { counterArray[0]++; });
}));
shouldBe(counterArray[0], 2000);

// --- Atomics on array elements (indices are property names) ---

const atomicArray = [0, 100];
shouldBe(Atomics.load(atomicArray, 0), 0);
shouldBe(Atomics.store(atomicArray, 0, 5), 5);
shouldBe(Atomics.add(atomicArray, 0, 2), 5);
shouldBe(atomicArray[0], 7);
shouldBe(Atomics.exchange(atomicArray, 1, 200), 100);
shouldBe(Atomics.compareExchange(atomicArray, 1, 200, 300), 200);
shouldBe(atomicArray[1], 300);

joinAll(spawnN(4, () => {
    for (let i = 0; i < 500; ++i)
        Atomics.add(atomicArray, 0, 1);
}));
shouldBe(atomicArray[0], 2007);

// --- A thread returns a freshly allocated array; spawner can use it ---

const produced = new Thread(() => {
    const fresh = [];
    for (let i = 0; i < 64; ++i)
        fresh[i] = i * i;
    return fresh;
}).join();
shouldBe(produced.length, 64);
shouldBe(produced[8], 64);
new Thread(arr => { arr[8] = -1; }, produced).join();
shouldBe(produced[8], -1);

// --- Chained sharing: thread A's writes are seen by thread B ---

const relay = [0, 0, 0];
new Thread(arr => { arr[0] = 1; arr[1] = 2; arr[2] = 3; }, relay).join();
shouldBe(new Thread(arr => arr[0] + arr[1] + arr[2], relay).join(), 6);
