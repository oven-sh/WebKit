function assert(c, m) { if (!c) throw new Error('assertion failed: ' + m); }

function waitUntil(cond, what) {
    const deadline = $.agent.monotonicNow() + 30000;
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
    const deadline = $.agent.monotonicNow() + 30000;
    while (ready < count) {
        const report = $.agent.getReport();
        if (report === 'ready') ready++;
        else if (report) throw new Error(report);
        else if ($.agent.monotonicNow() > deadline) throw new Error('timed out waiting for the agents to start');
        else $.agent.sleep(2);
    }
    $.agent.sleep(20);
}

shared struct Inner { value; }
shared struct Shared { counter; done; message; inner; made; }

const AGENTS = 4, ITERATIONS = 2000;
const shared = new Shared();
const inner = new Inner();
shared.counter = 0;
shared.done = 0;
shared.message = "from main";
shared.inner = inner;
shared.made = null;
inner.value = "from main";

// Every agent counts with a compareExchange loop, churns the string field while the main agent reads it,
// writes through the nested struct, and publishes a struct that it made itself.
const source = `
    $.agent.receiveBroadcast(function (handle) {
        try {
            const s = sharedStructFromHandle(handle);
            for (let i = 0; i < ${ITERATIONS}; i++) {
                for (;;) {
                    const current = Atomics.load(s, "counter");
                    if (Atomics.compareExchange(s, "counter", current, current + 1) === current)
                        break;
                }
                if (i % 50 === 0)
                    s.message = "agent message " + i;
            }
            s.inner.value = "written by an agent";
            shared struct Made { n; }
            const made = new Made();
            made.n = 42;
            Atomics.compareExchange(s, "made", null, made);
            for (;;) {
                const current = Atomics.load(s, "done");
                if (Atomics.compareExchange(s, "done", current, current + 1) === current)
                    break;
            }
        } catch (e) { $.agent.report('agent error: ' + e); }
        $.agent.leaving();
    });
`;
startAgents(AGENTS, source);
$.agent.broadcast(sharedStructHandle(shared, AGENTS));

let reads = 0;
waitUntil(() => {
    const message = shared.message;
    assert(typeof message === "string" && (message === "from main" || message.startsWith("agent message ")), "a torn read: " + message);
    reads++;
    return Atomics.load(shared, "done") >= AGENTS;
}, 'the agents to finish');

assert(shared.counter === AGENTS * ITERATIONS, "no lost update: " + shared.counter);
assert(shared.message.startsWith("agent message "), "a string written by an agent is visible");
assert(shared.inner === inner, "the nested struct keeps its identity");
assert(inner.value === "written by an agent", "a write by an agent through a nested struct is visible");
assert(shared.made !== null && shared.made.n === 42, "a struct made by an agent is usable here");
assert(Object.getPrototypeOf(shared.made) === null && Object.keys(shared.made).join() === "n", "its shape came along");
assert(shared.made === shared.made, "and keeps its identity");
