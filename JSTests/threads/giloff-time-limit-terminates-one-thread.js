//@ requireOptions("--useJSThreads=1", "--useThreadGIL=0", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1", "--useDollarVM=1")
// A time limit on a call (VM::addTerminationDeadline, which node:vm's timeout
// uses) terminates the thread that made the call. GIL-off, other threads keep
// running: the limit used to raise a VM-wide termination, which stopped every
// thread of the VM (audit row VM-9).

const SPINNERS = 3;
const ROUNDS = 5;

function expectTimeout(what) {
    try {
        $vm.callWithTimeLimit(() => { for (;;) { } }, 20);
    } catch (e) {
        if (e instanceof RangeError && e.message === "timed out")
            return;
        throw e;
    }
    throw new Error(what + ": the limited call returned");
}

// A limit that passes while its call sleeps in native code, when the call
// returns before any trap check: the call completes, and the request is
// withdrawn. The thread's request flag stood for "requested" rather than
// "handled", so this call timed out.
function expectCompletion(what) {
    let returned = false;
    $vm.callWithTimeLimit(() => { sleepSeconds(0.05); returned = true; }, 10);
    if (!returned)
        throw new Error(what + ": the call did not complete");
    if ($vm.hasPendingTermination())
        throw new Error(what + ": a request is still pending");
}

const state = { stop: false, started: 0 };
const spinners = [];
for (let i = 0; i < SPINNERS; ++i) {
    spinners.push(new Thread(() => {
        Atomics.add(state, "started", 1);
        let count = 0;
        while (!state.stop)
            ++count;
        return count;
    }));
}
while (Atomics.load(state, "started") !== SPINNERS) { }

// The main thread's limit stops the main thread's call only.
for (let round = 0; round < ROUNDS; ++round)
    expectTimeout("main thread, round " + round);
expectCompletion("main thread");

// A spawned thread's limit stops that thread's call only, and the main thread
// keeps running meanwhile.
const limited = new Thread(() => {
    for (let round = 0; round < ROUNDS; ++round)
        expectTimeout("spawned thread, round " + round);
    expectCompletion("spawned thread");
    return "done";
});
let mainCount = 0;
for (let i = 0; i < 1e6; ++i)
    mainCount += i & 1;
if (limited.join() !== "done")
    throw new Error("the limited thread did not finish");

state.stop = true;
for (const spinner of spinners) {
    const count = spinner.join();
    if (typeof count !== "number" || count <= 0)
        throw new Error("a spinner returned " + count);
}
