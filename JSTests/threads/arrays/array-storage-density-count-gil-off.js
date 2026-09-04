//@ requireOptions("--useJSThreads=1", "--useThreadGIL=0", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1", "--useDollarVM=1")
// An ArrayStorage array keeps a count of the values in its vector. A store
// into a hole of the vector updates the count under the cell lock, and a
// store past the vector reads the count to decide whether to grow the
// vector. GIL-off, the two stores can run on two threads, so the read takes
// the lock too. TSAN reports the race if it does not. The values must all
// land either way.

const ROUNDS = 200;
const SIZE = 64;

for (let round = 0; round < ROUNDS; ++round) {
    // An ArrayStorage vector of SIZE holes.
    const array = new Array(SIZE).fill(0);
    $vm.ensureArrayStorage(array);
    for (let i = 0; i < SIZE; ++i)
        delete array[i];

    const inside = new Thread(() => {
        for (let i = 0; i < SIZE; ++i)
            array[i] = i;
    });
    const beyond = new Thread(() => {
        for (let i = SIZE; i < 4 * SIZE; ++i)
            array[i] = i;
    });
    inside.join();
    beyond.join();

    for (let i = 0; i < 4 * SIZE; ++i) {
        if (array[i] !== i)
            throw new Error("round " + round + ": array[" + i + "] is " + array[i]);
    }
}
