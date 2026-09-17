//@ requireOptions("--useJSThreads=1", "--forceEagerCompilation=1")
// SPEC-jit history §56 (tenth round). --forceEagerCompilation (like
// --useConcurrentJIT=0) promises that a function is compiled and installed when
// the tier-up call returns; tests count compilations right after a loop. GIL off
// a compilation cannot run on a mutator thread (it would hold heap access, with
// no poll, for its whole length), and until the tenth round the process exited
// with a FATAL line when an option asked for it: thirty-nine stress tests in
// every configuration of the GIL-off JSC suite, 234 results. Now the request
// sets a derived option and the requesting thread waits, parked, until the
// concurrent JIT has finished its plan, and completes it before it returns.
// GIL on the compilation runs on the requesting thread (history §49) and the
// same checks hold.
// Checked: right after a short hot loop the function has been DFG-compiled - on
// the main thread, and on three threads that tier up their own functions at the
// same moment (their waits and completions overlap: one thread's completion takes
// every ready plan of the VM, another requester's too, which then has to wait for
// its key to leave that thread's hands - the first build did not, 7 of 1,200
// loaded runs) while a fourth keeps requesting collections; then on three threads
// that tier up one shared function (the later requests are duplicates of the
// first, and wait for its key).
load("../harness.js", "caller relative");

function makeWork(tag) {
    // A fresh function (and CodeBlock) per caller; the tag keeps the sources distinct.
    return new Function("n", "let s = 0; for (let i = 0; i < n; ++i) s += (i ^ " + tag + ") & 7; return s;");
}

function tierUpAndCheck(tag) {
    const work = makeWork(tag);
    noInline(work);
    let expected = work(100);
    for (let i = 0; i < 200; ++i) {
        if (work(100) !== expected)
            throw new Error("wrong result at call " + i + " (tag " + tag + ")");
    }
    const compiles = numberOfDFGCompiles(work);
    if (!(compiles >= 1))
        throw new Error("after 200 hot calls the function has " + compiles + " DFG compilations (tag " + tag + ")");
    return compiles;
}

tierUpAndCheck(1);

const gate = { go: 0, stop: 0 };
const collector = new Thread(() => {
    let n = 0;
    while (!Atomics.load(gate, "stop")) {
        gc();
        ++n;
        sleepMs(1);
    }
    return n;
});
const threads = [];
for (let t = 0; t < 3; ++t) {
    threads.push(new Thread((id) => {
        while (!Atomics.load(gate, "go")) { }
        let total = 0;
        for (let round = 0; round < 12; ++round)
            total += tierUpAndCheck(1000 + id * 100 + round);
        return total;
    }, t));
}
Atomics.store(gate, "go", 1);
for (const thread of threads)
    thread.join();

// One shared function, three threads: whoever asks second finds the key in flight.
for (let round = 0; round < 6; ++round) {
    const shared = makeWork(5000 + round);
    noInline(shared);
    const expected = shared(100);
    const sharedGate = { go: 0 };
    const sharers = [];
    for (let t = 0; t < 3; ++t) {
        sharers.push(new Thread(() => {
            while (!Atomics.load(sharedGate, "go")) { }
            for (let i = 0; i < 200; ++i) {
                if (shared(100) !== expected)
                    throw new Error("wrong result from the shared function at call " + i);
            }
            const compiles = numberOfDFGCompiles(shared);
            if (!(compiles >= 1))
                throw new Error("after 200 hot calls the shared function has " + compiles + " DFG compilations (round " + round + ")");
            return compiles;
        }));
    }
    Atomics.store(sharedGate, "go", 1);
    for (const thread of sharers)
        thread.join();
}
Atomics.store(gate, "stop", 1);
collector.join();
