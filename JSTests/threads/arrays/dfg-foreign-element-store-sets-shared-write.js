//@ requireOptions("--useJSThreads=1", "--useThreadGIL=0", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1")
// A store by a thread that does not own an array must set the array's
// shared-write bit before it lands (SPEC-jit section 5.5). While the bit is
// clear, the owner grows the array lock-free, and a growth copies the elements
// into a new butterfly. A foreign store to the old butterfly is then lost.
// The DFG and the FTL stored elements with no write predicate (audit row OM-9).
//
// The writer thread is the only thread that stores to the first SLOTS indices,
// so it must read back what it stored. The owner only appends.

const ROUNDS = 2000;
const SLOTS = 8;

// A separate function, so the compiler cannot forward the stored value.
function read(array, i) { return array[i]; }
noInline(read);

function writeAndCheck(array, round) {
    for (let i = 0; i < SLOTS; ++i) {
        const value = round * SLOTS + i;
        array[i] = value;
        const seen = read(array, i);
        if (seen !== value)
            throw new Error("round " + round + ": array[" + i + "] is " + seen + " after a store of " + value);
    }
}

// Tier up the writer while the arrays are still local to this thread.
for (let i = 0; i < 20000; ++i)
    writeAndCheck(new Array(SLOTS).fill(0), i);

// Each round starts a small array, so the owner's pushes grow it many times.
const state = { stop: false, started: false, array: new Array(SLOTS).fill(0) };
const writer = new Thread(() => {
    let count = 0;
    state.started = true;
    while (!state.stop)
        writeAndCheck(state.array, ++count);
    return count;
});
while (!state.started) { }

for (let round = 0; round < ROUNDS; ++round) {
    const array = new Array(SLOTS).fill(0);
    state.array = array;
    for (let i = 0; i < 4000; ++i)
        array.push(i);
}
state.stop = true;
writer.join();
