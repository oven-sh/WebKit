// Stress: blocks are created and dropped by the main agent and by other agents, in volume, and nothing is left
// behind. Blocks are reference counted, so what must hold is that every kind of drop releases its references:
// a wrapper that is collected, a slot that is overwritten, a long chain of blocks that is dropped at once (which
// must not recurse once per link), a reference cycle (one the program has broken, and one that nobody breaks, which
// the cycle collector reclaims: see shared-cycles.js for its own tests), and blocks that other agents publish and
// replace. The process memory is read from /proc/self/status where that exists (the workflow runs on Linux): each
// phase would leave hundreds of megabytes behind if its blocks leaked, and may grow by 150 MB at most.
function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }

function rssMegabytes() {
    // The files of /proc report a size of zero, which readFile() takes for empty, so read them line by line.
    try {
        const file = openFile("/proc/self/status");
        for (let i = 0; i < 100; i++) {
            const match = /^VmRSS:\s+(\d+) kB/.exec(readline(file));
            if (match)
                return Number(match[1]) / 1024;
        }
    } catch (e) { }
    return undefined;
}
const LIMIT = 150;
let baseline = rssMegabytes();
function checkMemory(phase) {
    gc();
    const now = rssMegabytes();
    if (now === undefined || baseline === undefined) {
        print("# " + phase + ": no process memory information, memory not checked");
        return;
    }
    const growth = now - baseline;
    print("# " + phase + ": process memory " + now.toFixed(0) + " MB, " + growth.toFixed(0) + " MB above the start");
    assert(growth < LIMIT, phase + " left " + growth.toFixed(0) + " MB behind");
    baseline = now;
}

shared struct Box { text; next; }
const text = "block text ".repeat(200); // about 2200 characters, 4.4 KB as UTF-16 in a block

// 1. Create and drop 100000 blocks that each hold a string. A leak would be 440 MB.
for (let i = 0; i < 100000; i++) {
    const box = new Box();
    box.text = text;
    if (i % 5000 === 0)
        gc();
}
checkMemory("create and drop");

// 2. One struct overwritten 300000 times with fresh strings and fresh nested blocks: every old value is released.
{
    const box = new Box();
    for (let i = 0; i < 300000; i++) {
        box.text = text + i;
        const nested = new Box();
        nested.text = text;
        box.next = nested;
        if (i % 20000 === 0)
            gc();
    }
    assert(box.next.text === text && box.text === text + 299999, "the last values");
}
checkMemory("overwrite");

// 3. A chain of 300000 blocks dropped at once: releasing the head releases the rest, iteratively.
{
    let head = undefined;
    for (let i = 0; i < 300000; i++) {
        const link = new Box();
        link.text = "link " + i;
        link.next = head;
        head = link;
        if (i % 50000 === 0)
            gc();
    }
    assert(head.text === "link 299999", "the head of the chain");
    let length = 0;
    for (let link = head; link; link = link.next)
        length++;
    assert(length === 300000, "the chain is whole: " + length);
    head = undefined;
}
checkMemory("long chain");

// 4. Reference cycles that are broken before they are dropped are reclaimed.
for (let i = 0; i < 20000; i++) {
    const a = new Box();
    const b = new Box();
    a.text = text;
    b.text = text;
    a.next = b;
    b.next = a;
    assert(a.next.next === a, "a cycle");
    b.next = undefined;
    if (i % 2000 === 0)
        gc();
}
checkMemory("broken cycles");

// 4b. Reference cycles that nobody breaks: the collector reclaims them (here on demand, and in the background too).
for (let i = 0; i < 20000; i++) {
    const a = new Box();
    const b = new Box();
    a.text = text;
    b.text = text;
    a.next = b;
    b.next = a;
    assert(a.next.next === a, "an unbroken cycle");
    if (i % 2000 === 0) {
        gc();
        collectSharedCycles();
    }
}
gc();
collectSharedCycles();
checkMemory("unbroken cycles");

// 5. Other agents create blocks, publish each one in a slot of a shared struct (replacing the one before, which
// sometimes links to it, so chains form and fall) while the main agent reads the slot.
shared struct Board { slot; done; }
const AGENTS = 3;
const ITERATIONS = 20000;
const board = new Board();
board.slot = undefined;
board.done = 0;

const source = `
    $.agent.receiveBroadcast(function (handle) {
        try {
            const s = sharedStructFromHandle(handle);
            shared struct Box { text; next; }
            const text = "block text ".repeat(200);
            for (let i = 0; i < ${ITERATIONS}; i++) {
                const box = new Box();
                box.text = text;
                if (i % 7 === 0)
                    box.next = s.slot;
                s.slot = box;
                const dropped = new Box();
                dropped.text = text;
                if (i % 2000 === 0)
                    gc();
            }
            for (;;) {
                const current = Atomics.load(s, "done");
                if (Atomics.compareExchange(s, "done", current, current + 1) === current)
                    break;
            }
        } catch (e) { $.agent.report('agent error: ' + e); }
        $.agent.leaving();
    });
`;
for (let i = 0; i < AGENTS; i++)
    $.agent.start("$.agent.report('ready');" + source);
let ready = 0;
{
    const deadline = $.agent.monotonicNow() + 30000;
    while (ready < AGENTS) {
        const report = $.agent.getReport();
        if (report === "ready") ready++;
        else if (report) throw new Error(report);
        else if ($.agent.monotonicNow() > deadline) throw new Error("timed out waiting for the agents to start");
        else $.agent.sleep(2);
    }
    $.agent.sleep(20);
}
$.agent.broadcast(sharedStructHandle(board, AGENTS));
{
    const deadline = $.agent.monotonicNow() + 90000;
    let reads = 0;
    while (Atomics.load(board, "done") < AGENTS) {
        const report = $.agent.getReport();
        if (report && report !== "ready") throw new Error(report);
        if ($.agent.monotonicNow() > deadline) throw new Error("timed out waiting for the agents to finish");
        const box = board.slot;
        if (box !== undefined) {
            assert(box.text === text, "a block published by an agent holds its string");
            reads++;
        }
    }
    print("# agents: " + reads + " reads of published blocks");
}
board.slot = undefined;
$.agent.sleep(50); // let the agent threads wind down
checkMemory("agents");
