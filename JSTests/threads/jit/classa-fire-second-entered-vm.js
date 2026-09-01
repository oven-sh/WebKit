//@ requireOptions("--useJSThreads=1")
// Two independent VMs in one process: the jsc shell's $.agent runs a second VM
// on its own thread. A Class-A watchpoint fire (the second write to a global
// lexical binding, the first transition away from a fresh Structure) stops the
// world of the firing VM only: under useJSThreads that VM's JSLock already
// makes the caller the sole mutator of its code, so another VM that is entered
// at the same time is neither counted nor parked. Each VM fires while the
// other is entered and spinning.

const sab = new SharedArrayBuffer(16);
const flags = new Int32Array(sab);
const AGENT_ENTERED = 0;
const AGENT_FIRED = 1;
const MAIN_DONE = 2;

function fireClassASets(prefix) {
    for (let i = 0; i < 100; i++) {
        // Fresh Structure from the empty shape, then its first transition.
        const o = {};
        o[prefix + i] = i;
        o.second = i;
    }
}

$.agent.start(`
    let counter = 0;
    ${fireClassASets.toString()}
    $.agent.receiveBroadcast((sab) => {
        const flags = new Int32Array(sab);
        Atomics.store(flags, ${AGENT_ENTERED}, 1);
        // Main is entered, spinning on AGENT_FIRED, during every fire below.
        counter = 1; // Second write to the global lexical: fires its set.
        fireClassASets("agentProp");
        Atomics.store(flags, ${AGENT_FIRED}, 1);
        while (Atomics.load(flags, ${MAIN_DONE}) === 0) {}
        $.agent.report("done:" + counter);
    });
`);
$.agent.broadcast(sab);
while (Atomics.load(flags, AGENT_ENTERED) === 0) {}
while (Atomics.load(flags, AGENT_FIRED) === 0) {}

// The agent is entered, spinning on MAIN_DONE, during every fire below.
let counter = 0;
counter = 1;
fireClassASets("mainProp");

Atomics.store(flags, MAIN_DONE, 1);
let report;
while (!(report = $.agent.getReport())) {}
if (report !== "done:1")
    throw new Error("unexpected agent report: " + report);
