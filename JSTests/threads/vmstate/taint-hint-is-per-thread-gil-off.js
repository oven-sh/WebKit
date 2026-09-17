//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
//@ threadsRequireGILOff
// SPEC-ungil history, tenth round (K4.II.15). The "tainted code may have run in
// this synchronous execution" hint filters a stack walk: set on entry to code
// that could be tainted, cleared when a synchronous execution ends. The K4
// inventory ruled it per thread; the implementation kept one VM byte and made it
// sticky GIL off, so after any tainted code had run every thread answered
// IndirectlyTaintedByHistory for the rest of the process
// (stress/taintedness-tracking.js, all sixteen GIL-off configurations). GIL off
// it now lives in the lite: every tier stores through the running thread's
// lite, and the end of an execution clears the ending thread's hint only.
// Checked: a thread that never ran tainted code answers Untainted while another
// thread is inside tainted code, and after it; the tainting thread sees its own
// history; tainted code made hot (so the Baseline, DFG and FTL prologues do the
// store) marks only the thread that runs it; and the main thread's own mark
// goes away when its turn ends.
load("../harness.js", "caller relative");

function state() { return $vm.vmTaintedState(); }
function expect(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": " + actual + ", expected " + expected);
}

expect(state(), "Untainted", "main before anything");

// (1) Another thread is inside tainted code; this one is not.
const gate = { phase: 0, inside: null };
const tainting = new Thread(() => {
    $vm.runTaintedString(`
        Atomics.store(gate, "phase", 1);
        while (Atomics.load(gate, "phase") < 2) { }
        gate.inside = $vm.vmTaintedState();
    `);
    return $vm.vmTaintedState(); // The same synchronous execution: its own history.
});
while (Atomics.load(gate, "phase") < 1) { }
expect(state(), "Untainted", "main while another thread is inside tainted code");
expect(new Thread(() => $vm.vmTaintedState()).join(), "Untainted", "a fresh thread while another thread is inside tainted code");
Atomics.store(gate, "phase", 2);
expect(tainting.join(), "IndirectlyTaintedByHistory", "the tainting thread after its tainted code returned");
expect(gate.inside, "KnownTainted", "inside the tainted code");
expect(state(), "Untainted", "main after the tainting thread finished");
expect(new Thread(() => $vm.vmTaintedState()).join(), "Untainted", "a fresh thread after the tainting thread finished");

// (2) The same through the JIT tiers' prologue stores: a tainted function made hot on one thread.
const hot = new Thread(() => {
    $vm.runTaintedString("globalThis.taintedHot = function (x) { return x + 1; };");
    let sum = 0;
    for (let i = 0; i < 400000; ++i)
        sum += taintedHot(i);
    return { sum, state: $vm.vmTaintedState() };
});
const cold = new Thread(() => {
    let last = "Untainted";
    for (let i = 0; i < 2000; ++i) {
        last = $vm.vmTaintedState();
        if (last !== "Untainted")
            break;
    }
    return last;
});
const hotResult = hot.join();
expect(hotResult.sum, 80000200000, "the hot loop's sum");
expect(hotResult.state, "IndirectlyTaintedByHistory", "the thread that ran the hot tainted function");
expect(cold.join(), "Untainted", "a thread that ran beside it");
expect(state(), "Untainted", "main beside it");

// (3) The main thread's own mark ends with its turn.
$vm.runTaintedString("1");
expect(state(), "IndirectlyTaintedByHistory", "main in the turn that ran tainted code");
setTimeout(() => {
    expect(state(), "Untainted", "main in the next turn");
});
