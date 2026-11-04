// notify(cv, count) wakes exactly count of the waiters that were waiting when it ran, and answers how many: a waiter that
// holds the mutex when it registers is counted by a notify that runs under the mutex afterwards, a count of 0 wakes
// nobody, and no waiter is woken twice or by a notify that did not count it.
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

const AGENTS = 5;
const mutex = new Atomics.Mutex();
const cond = new Atomics.Condition();
const state = new Int32Array(new SharedArrayBuffer(16)); // [waiting, woken, release, 0]

const source = `
    $.agent.receiveBroadcast(function (mutexState) {
        const m = atomicsSyncFromState(mutexState, 0);
        $.agent.receiveBroadcast(function (conditionState) {
            const c = atomicsSyncFromState(conditionState, 1);
            $.agent.receiveBroadcast(function (sab) {
                try {
                    const s = new Int32Array(sab);
                    const t = Atomics.Mutex.lock(m);
                    s[0]++;                        // counted under the mutex, and so registered by the time it is released
                    Atomics.Condition.wait(c, t);  // registers as a waiter, then releases the mutex
                    Atomics.add(s, 1, 1);
                    t.unlock();
                } catch (e) { $.agent.report('agent error: ' + e); }
                $.agent.leaving();
            });
        });
    });
`;
startAgents(AGENTS, source);
$.agent.broadcast(atomicsSyncState(mutex));
$.agent.broadcast(atomicsSyncState(cond));
$.agent.broadcast(state.buffer);

function everyoneWaiting() {
    using t = Atomics.Mutex.lock(mutex);
    return state[0] === AGENTS;
}
waitUntil(everyoneWaiting, 'the agents to wait');

function notifyUnderTheMutex(count) {
    using t = Atomics.Mutex.lock(mutex);
    return Atomics.Condition.notify(cond, count);
}
function woken() {
    $.agent.sleep(100);
    return Atomics.load(state, 1);
}

assert(notifyUnderTheMutex(0) === 0 && woken() === 0, "a count of 0 wakes nobody");
assert(notifyUnderTheMutex(1) === 1, "notify(cv, 1) answers 1");
assert(woken() === 1, "and wakes one waiter, not two: " + Atomics.load(state, 1));
assert(notifyUnderTheMutex(2) === 2, "notify(cv, 2) answers 2");
assert(woken() === 3, "and wakes two more: " + Atomics.load(state, 1));
assert(notifyUnderTheMutex(Infinity) === AGENTS - 3, "notify(cv, Infinity) answers the number that is left");
waitUntil(() => Atomics.load(state, 1) === AGENTS, 'the agents to wake');
assert(notifyUnderTheMutex(1) === 0 && notifyUnderTheMutex(Infinity) === 0, "nobody is left");
