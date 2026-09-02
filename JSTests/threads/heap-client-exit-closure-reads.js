//@ requireOptions("--useJSThreads=1")
// Each call of phase(tag) makes new closures over tag, and each closure runs on
// its own thread. A thread must only ever read its own call's tag.
//
// With the GIL off, this failed when a thread's exit left its heap client with
// StopAllocatingMode::ForGood. That stop does not record the cells allocated
// since the last collection in the newly-allocated bitmap, so those cells
// looked dead to the heap that kept running. In the next round the threads'
// optimized code read tag as 0, the value of the round before. The stop must be
// the resumable one (LocalAllocator::stopAllocatingForClientTeardown).
//
// The failure needs the optimizing tiers: it did not happen with the DFG off.
load("./harness.js", "caller relative");

const COPIES = 4;
const ROUNDS = 4;
const THREADS = 3;
const ITERATIONS = 300000;

// The failure shows up in one round transition per function, and not in
// every process, so the test runs several copies of the function. Each copy
// has its own source text, so it gets its own code.
function phaseSource(copy) {
    return `(function phase(tag, expected) {
        // copy ${copy}
        const threads = [];
        for (let t = 0; t < ${THREADS}; ++t) {
            threads.push(new Thread(() => {
                let wrong = 0;
                let firstWrong;
                for (let i = 0; i < ${ITERATIONS}; ++i) {
                    const v = tag + t;
                    if (v !== tag + t || v - t !== expected[0]) {
                        if (!wrong)
                            firstWrong = v - t;
                        wrong++;
                    }
                }
                return [wrong, firstWrong];
            }));
        }
        return joinAll(threads);
    })`;
}

for (let copy = 0; copy < COPIES; ++copy) {
    const phase = eval(phaseSource(copy));
    const expected = [0];
    for (let round = 0; round < ROUNDS; ++round) {
        expected[0] = round * 10;
        const results = phase(round * 10, expected);
        for (let t = 0; t < THREADS; ++t) {
            const [wrong, firstWrong] = results[t];
            if (wrong)
                throw new Error("copy " + copy + ", round " + round + ", thread " + t + ": read tag " + firstWrong + " instead of " + (round * 10) + " in " + wrong + " iterations");
        }
    }
}
