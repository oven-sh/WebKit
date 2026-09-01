//@ requireOptions("--useJSThreads=1", "--thresholdForJITAfterWarmUp=20", "--thresholdForOptimizeAfterWarmUp=100")
// Two independent VMs in one process: the jsc shell's $.agent runs a second VM
// on its own thread. Under useJSThreads each VM's JSLock already makes the
// caller the sole mutator of that VM's code, so a reoptimization jettison in
// one VM must not require every other VM in the process to be idle. The second
// VM stays entered (spinning on a shared flag) while the first one tiers up
// and jettisons.

const sab = new SharedArrayBuffer(8);
const flags = new Int32Array(sab);
const RELEASE = 0;
const ENTERED = 1;

$.agent.start(`
    $.agent.receiveBroadcast((sab) => {
        const flags = new Int32Array(sab);
        Atomics.store(flags, ${ENTERED}, 1);
        while (Atomics.load(flags, ${RELEASE}) === 0) {}
        $.agent.report("done");
    });
`);
$.agent.broadcast(sab);
while (Atomics.load(flags, ENTERED) === 0) {}

function churn() {
    function f(o) { return o.x + 1; }
    let sum = 0;
    for (let i = 0; i < 20000; i++)
        sum += f({ x: i });
    for (let i = 0; i < 20000; i++)
        sum += f({ y: 1, x: i });
    for (let i = 0; i < 20000; i++)
        sum += f(i & 1 ? { z: 2, y: 1, x: i } : { x: i });
    return sum;
}

let expected = null;
for (let round = 0; round < 4; round++) {
    const sum = churn();
    if (expected === null)
        expected = sum;
    else if (sum !== expected)
        throw new Error("torn result after jettison: " + sum + " !== " + expected);
}

Atomics.store(flags, RELEASE, 1);
let report;
while (!(report = $.agent.getReport())) {}
if (report !== "done")
    throw new Error("unexpected agent report: " + report);
