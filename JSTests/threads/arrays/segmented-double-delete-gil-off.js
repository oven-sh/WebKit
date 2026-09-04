//@ requireOptions("--useJSThreads=1", "--useThreadGIL=0", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1")
// A double array that several threads write has a segmented butterfly, and its
// elements are raw doubles. A delete stores the hole (a NaN) while another
// thread reads the element, so the store is a relaxed atomic, like the read.
// TSAN reports the race if it is not. A read sees the number or the hole.

const ROUNDS = 100;
const SIZE = 64;

for (let round = 0; round < ROUNDS; ++round) {
    const array = [];
    for (let i = 0; i < SIZE; ++i)
        array.push(i + 0.5);

    // A write from another thread, then a new property: the butterfly grows
    // while it is shared-written, so it is segmented from here on.
    new Thread(() => {
        array[0] = 0.5;
        array.grown = round;
    }).join();

    const reader = new Thread(() => {
        let seen = 0;
        for (let pass = 0; pass < 20; ++pass) {
            for (let i = 0; i < SIZE; ++i) {
                const value = array[i];
                if (value !== undefined && value !== i + 0.5)
                    throw new Error("round " + round + ": array[" + i + "] is " + value);
                if (value !== undefined)
                    ++seen;
            }
        }
        return seen;
    });
    for (let i = 0; i < SIZE; ++i)
        delete array[i];
    reader.join();

    for (let i = 0; i < SIZE; ++i) {
        if (i in array)
            throw new Error("round " + round + ": index " + i + " was not deleted");
    }
}
