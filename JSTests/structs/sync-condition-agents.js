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
const mutex = new Atomics.Mutex();
const cond = new Atomics.Condition();
const flags = new Int32Array(new SharedArrayBuffer(16)); // [ready, waiting, done, 0]

const source = `
    $.agent.receiveBroadcast(function (mutexState) {
        const m = atomicsSyncFromState(mutexState, 0);
        $.agent.receiveBroadcast(function (conditionState) {
            const c = atomicsSyncFromState(conditionState, 1);
            $.agent.receiveBroadcast(function (sab) {
                try {
                    const f = new Int32Array(sab);
                    const t = Atomics.Mutex.lock(m);
                    Atomics.store(f, 1, 1);
                    while (Atomics.load(f, 0) === 0)
                        Atomics.Condition.wait(c, t);
                    t.unlock();
                    Atomics.store(f, 2, 1);
                } catch (e) { $.agent.report('agent error: ' + e); }
                $.agent.leaving();
            });
        });
    });
`;
startAgents(1, source);
$.agent.broadcast(atomicsSyncState(mutex));
$.agent.broadcast(atomicsSyncState(cond));
$.agent.broadcast(flags.buffer);

waitUntil(() => Atomics.load(flags, 1) !== 0, 'the agent to wait');
{
    using t = Atomics.Mutex.lock(mutex);
    Atomics.store(flags, 0, 1);
    assert(Atomics.Condition.notify(cond) <= 1, "notify wakes at most the one waiter");
}
waitUntil(() => Atomics.load(flags, 2) !== 0, 'the agent to finish');
