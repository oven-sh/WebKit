function assert(c, m) { if (!c) throw new Error('assertion failed: ' + m); }

function waitUntil(cond, what) {
    const deadline = $.agent.monotonicNow() + 20000;
    while (!cond()) {
        const report = $.agent.getReport();
        if (report && report !== 'ready') throw new Error(report);
        if ($.agent.monotonicNow() > deadline) throw new Error('timed out waiting for ' + what);
        $.agent.sleep(2);
    }
}

function startAgents(count, source) {
    for (let i = 0; i < count; i++)
        $.agent.start("$.agent.report('ready');" + source);
    let ready = 0;
    const deadline = $.agent.monotonicNow() + 20000;
    while (ready < count) {
        const report = $.agent.getReport();
        if (report === 'ready') ready++;
        else if (report) throw new Error(report);
        else if ($.agent.monotonicNow() > deadline) throw new Error('timed out waiting for the agents to start');
        else $.agent.sleep(2);
    }
    $.agent.sleep(20);
}
const AGENTS = 4, ITERATIONS = 2000;
const mutex = new Atomics.Mutex();
const counter = new Int32Array(new SharedArrayBuffer(8)); // [count, finished agents]

const source = `
    $.agent.receiveBroadcast(function (mutexState) {
        const m = atomicsSyncFromState(mutexState, 0);
        $.agent.receiveBroadcast(function (sab) {
            try {
                const c = new Int32Array(sab);
                for (let i = 0; i < ${ITERATIONS}; i++) {
                    const t = Atomics.Mutex.lock(m);
                    c[0] = c[0] + 1;
                    t.unlock();
                }
                Atomics.add(c, 1, 1);
            } catch (e) { $.agent.report('agent error: ' + e); }
            $.agent.leaving();
        });
    });
`;
startAgents(AGENTS, source);
$.agent.broadcast(atomicsSyncState(mutex));
$.agent.broadcast(counter.buffer);
waitUntil(() => Atomics.load(counter, 1) >= AGENTS, 'agents to finish');
assert(counter[0] === AGENTS * ITERATIONS, "mutex protects the counter: " + counter[0]);
