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

const AGENTS = 4, ITERATIONS = 2000;
// Slots: 0 counter, 1 done, 2 message, 3 nested array, 4 an array made by an agent, 5 a nested shared struct.
shared struct Cell { value; }
const shared = new SharedArray(6);
const inner = new SharedArray(2);
const cell = new Cell();
shared[0] = 0;
shared[1] = 0;
shared[2] = "from main";
shared[3] = inner;
shared[4] = null;
shared[5] = cell;
inner[0] = "from main";
cell.value = "from main";

// Every agent counts with a compareExchange loop on element 0, churns the string element while the main agent
// reads it, writes through the nested array and struct, and publishes an array that it made itself.
const source = `
    $.agent.receiveBroadcast(function (handle) {
        try {
            const s = sharedStructFromHandle(handle);
            if (!(Object.getPrototypeOf(s) === null && s.length === 6))
                throw new Error("the handle did not give a SharedArray");
            for (let i = 0; i < ${ITERATIONS}; i++) {
                for (;;) {
                    const current = Atomics.load(s, "0");
                    if (Atomics.compareExchange(s, "0", current, current + 1) === current)
                        break;
                }
                if (i % 50 === 0)
                    s[2] = "agent message " + i;
            }
            s[3][0] = "written by an agent";
            s[5].value = "written by an agent";
            const made = new SharedArray(2);
            made[0] = 42;
            made[1] = s;
            Atomics.compareExchange(s, "4", null, made);
            for (;;) {
                const current = Atomics.load(s, "1");
                if (Atomics.compareExchange(s, "1", current, current + 1) === current)
                    break;
            }
        } catch (e) { $.agent.report('agent error: ' + e); }
        $.agent.leaving();
    });
`;
startAgents(AGENTS, source);
$.agent.broadcast(sharedArrayHandle(shared, AGENTS));

waitUntil(() => {
    const message = shared[2];
    assert(typeof message === "string" && (message === "from main" || message.startsWith("agent message ")), "a torn read: " + message);
    return Atomics.load(shared, "1") >= AGENTS;
}, 'the agents to finish');

assert(shared[0] === AGENTS * ITERATIONS, "no lost update: " + shared[0]);
assert(shared[2].startsWith("agent message "), "a string written by an agent is visible");
assert(shared[3] === inner, "the nested array keeps its identity");
assert(inner[0] === "written by an agent", "a write by an agent through a nested array is visible");
assert(shared[5] === cell && cell.value === "written by an agent", "and through a nested struct");
const made = shared[4];
assert(made !== null && made.length === 2 && made[0] === 42, "an array made by an agent is usable here");
assert(Object.getPrototypeOf(made) === null && Object.keys(made).join() === "0,1", "its shape came along");
assert(made === shared[4], "and keeps its identity");
assert(made[1] === shared, "it holds the array that was shared with it");
