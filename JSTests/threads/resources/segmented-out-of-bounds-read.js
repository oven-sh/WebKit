// The body of arrays/segmented-out-of-bounds-read*.js.
//
// A read past the end of a contiguous array takes the slow path of get_by_val
// (the LLInt's, or the JIT's). It read the array's length through butterfly(),
// which must not decode a segmented word. So a read of an array that another
// thread had segmented decoded the spine as a butterfly. With
// --verifyConcurrentButterfly, that fails an assertion. The slow paths read the
// length through the word now.

const ROUNDS = 3000;
const SIZE = 16;

const state = { array: null, stop: false, started: 0, grown: 0 };

const reader = new Thread(() => {
    Atomics.add(state, "started", 1);
    let count = 0;
    while (!state.stop) {
        const array = state.array;
        if (!array)
            continue;
        if (array[SIZE + 100] !== undefined)
            throw new Error("an index past the end has a value");
        ++count;
    }
    return count;
});

// A write from this thread, then a new property: the butterfly grows while it
// is shared-written, so it is segmented from then on.
const writer = new Thread(() => {
    Atomics.add(state, "started", 1);
    let done = 0;
    while (!state.stop) {
        const array = state.array;
        if (!array || array.grown !== undefined)
            continue;
        array[0] = "w";
        array.grown = ++done;
        Atomics.add(state, "grown", 1);
    }
    return done;
});

while (Atomics.load(state, "started") !== 2) { }

// A Debug build is much slower, so the rounds also stop at a deadline.
const deadline = Date.now() + 10000;
for (let round = 0; round < ROUNDS && Date.now() < deadline; ++round) {
    const array = [];
    for (let i = 0; i < SIZE; ++i)
        array.push("v" + i);
    state.array = array;
    while (Atomics.load(state, "grown") === round) { }
    if (array[1] !== "v1" || array[0] !== "w")
        throw new Error("round " + round + ": the elements changed");
}

state.stop = true;
if (!(reader.join() > 0))
    throw new Error("the reader did not run");
writer.join();
