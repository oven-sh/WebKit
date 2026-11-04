// Reference cycles between shared structs and SharedArrays are reclaimed. Blocks are reference counted, so a cycle
// (a holds b holds a, a block that holds itself, a ring, a cycle through SharedArray elements) is garbage that
// counting alone cannot free; the collector in SharedStructBlock.cpp finds it without the program breaking it.
//
// The hooks used here exist in the jsc shell only: liveSharedBlocks() and liveSharedStrings() count what exists in
// the process, collectSharedCycles() runs the collector now and returns the number of blocks it destroyed. The
// wrappers of the blocks are ordinary JS objects, so a cycle becomes garbage only once the garbage collector has
// finalized them: settle() runs both, and the stack is churned at several depths because the collector scans it
// conservatively.
function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }

shared struct Node { next; other; text; id; }
shared struct Ctl { cycle; release; released; ready; }

function gcDeep(n) {
    if (n > 0) {
        const pad = [n, n + 1, n + 2, n + 3];
        gcDeep(n - 1);
        return pad.length;
    }
    gc();
}

function settle(predicate, what) {
    for (let i = 0; i < 300; i++) {
        gcDeep((i % 9) * 15);
        collectSharedCycles();
        if (predicate())
            return;
        $.agent.sleep(5);
    }
    throw new Error("not reclaimed: " + what + " (blocks " + liveSharedBlocks() + ", strings " + liveSharedStrings() + ")");
}

function waitUntil(cond, what) {
    const deadline = $.agent.monotonicNow() + 60000;
    while (!cond()) {
        const report = $.agent.getReport();
        if (report && report !== "ready") throw new Error(report);
        if ($.agent.monotonicNow() > deadline) throw new Error("timed out waiting for " + what);
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
        if (report === "ready") ready++;
        else if (report) throw new Error(report);
        else if ($.agent.monotonicNow() > deadline) throw new Error("timed out waiting for the agents to start");
        else $.agent.sleep(2);
    }
    $.agent.sleep(20);
}

// Runs `create`, which builds cycles and returns how many blocks and strings it holds at its peak, and checks that
// everything is gone afterwards.
function expectReclaimed(name, create) {
    const blocks = liveSharedBlocks();
    const strings = liveSharedStrings();
    create(blocks, strings);
    settle(() => liveSharedBlocks() <= blocks && liveSharedStrings() <= strings, name);
    print("# " + name + ": reclaimed");
}

function ring(length, tag) {
    const nodes = [];
    for (let i = 0; i < length; i++) {
        const node = new Node();
        node.id = i;
        node.text = tag + " " + i;
        nodes.push(node);
    }
    for (let i = 0; i < length; i++)
        nodes[i].next = nodes[(i + 1) % length];
    return nodes[0];
}

function walk(head, steps) {
    let node = head;
    for (let i = 0; i < steps; i++)
        node = node.next;
    return node;
}

// 1. A block that holds itself.
expectReclaimed("self cycles", (blocks) => {
    const kept = [];
    for (let i = 0; i < 200; i++) {
        const node = new Node();
        node.id = i;
        node.text = "self " + i;
        node.next = node;
        node.other = node;
        assert(node.next === node && node.other.next.text === "self " + i, "a self cycle reads back");
        kept.push(node);
    }
    assert(liveSharedBlocks() >= blocks + 200, "the self cycles exist");
});

// 2. Two blocks that hold each other.
expectReclaimed("two-node cycles", (blocks, strings) => {
    const kept = [];
    for (let i = 0; i < 200; i++) {
        const a = new Node();
        const b = new Node();
        a.text = "a " + i;
        b.text = "b " + i;
        a.next = b;
        b.next = a;
        assert(a.next.next === a && b.next.next === b, "a two-node cycle reads back");
        kept.push(a, b);
    }
    assert(liveSharedBlocks() >= blocks + 400 && liveSharedStrings() >= strings + 400, "the cycles exist");
});

// 3. A ring of 1000 blocks, and a few more rings of other lengths.
expectReclaimed("rings", (blocks) => {
    const big = ring(1000, "big");
    assert(walk(big, 1000) === big && walk(big, 999).next === big, "the ring of 1000 closes");
    const rings = [];
    for (let length = 3; length < 40; length += 4)
        rings.push(ring(length, "ring"));
    assert(liveSharedBlocks() >= blocks + 1000, "the rings exist");
});

// 4. Cycles through the elements of a SharedArray.
expectReclaimed("SharedArray cycles", (blocks) => {
    const kept = [];
    for (let i = 0; i < 100; i++) {
        const array = new SharedArray(3);
        const node = new Node();
        node.text = "node " + i;
        array[0] = node;
        array[1] = array;
        array[2] = "element " + i;
        node.next = array;
        assert(array[0].next === array && array[1][1] === array && node.next[0] === node, "a SharedArray cycle reads back");
        kept.push(array);
    }
    // An array of arrays that holds the first one again.
    const first = new SharedArray(2);
    let last = first;
    for (let i = 0; i < 50; i++) {
        const next = new SharedArray(2);
        last[0] = next;
        last = next;
    }
    last[0] = first;
    assert(liveSharedBlocks() >= blocks + 200, "the SharedArray cycles exist");
});

// 5. Cycles that own strings and BigInts: all of them are freed with the blocks.
expectReclaimed("strings and BigInts", (blocks, strings) => {
    const kept = [];
    for (let i = 0; i < 100; i++) {
        const a = new Node();
        const b = new Node();
        a.text = "long text of a block ".repeat(20) + i;
        b.text = "long text of another block ".repeat(20) + i;
        a.id = 123456789012345678901234567890n + BigInt(i);
        b.id = -987654321098765432109876543210n - BigInt(i);
        a.next = b;
        b.next = a;
        assert(a.id > 0n && b.id < 0n && a.next.text.length > 400, "the values read back");
        kept.push(a);
    }
    assert(liveSharedStrings() >= strings + 400, "the strings are owned by the blocks");
});

// 6. A cycle that is still referenced is not collected, and is once the reference goes. A cycle that hangs off a
// live struct, and one of which only a single member is held, are alive too.
let keep = ring(50, "kept");
let tail = new Node();
tail.next = ring(20, "hung");
{
    const before = liveSharedBlocks();
    assert(before >= 70, "the kept cycles exist");
    for (let i = 0; i < 25; i++) {
        gcDeep((i % 9) * 15);
        collectSharedCycles();
        $.agent.sleep(2);
    }
    assert(liveSharedBlocks() >= before, "a referenced cycle is not collected: " + liveSharedBlocks() + " < " + before);
    assert(walk(keep, 50) === keep && keep.text === "kept 0", "the kept ring is intact");
    assert(walk(tail.next, 20) === tail.next && tail.next.text === "hung 0", "the hung ring is intact");
    let member = walk(keep, 25);
    keep = undefined;
    for (let i = 0; i < 25; i++) {
        gcDeep((i % 9) * 15);
        collectSharedCycles();
        $.agent.sleep(2);
    }
    assert(liveSharedBlocks() >= before - 1 && walk(member, 50) === member && member.text === "kept 25", "one held member keeps its ring: " + liveSharedBlocks());
    member = undefined;
    tail = undefined;
    settle(() => liveSharedBlocks() <= before - 70, "the kept cycles once released");
}

// 7. A cycle that another agent holds is not collected while it does, and is when it lets go.
{
    const blocks = liveSharedBlocks();
    const ctl = new Ctl();
    ctl.release = 0;
    ctl.released = 0;
    ctl.ready = 0;
    ctl.cycle = ring(5, "agent");
    const source = `
        shared struct Node { next; other; text; id; }
        shared struct Ctl { cycle; release; released; ready; }
        function gcDeep(n) {
            if (n > 0) {
                const pad = [n, n + 1, n + 2, n + 3];
                gcDeep(n - 1);
                return pad.length;
            }
            gc();
        }
        function acquire(c) { globalThis.held = c.cycle; }
        function drop() { globalThis.held = undefined; }
        $.agent.receiveBroadcast(function (handle) {
            try {
                const c = sharedStructFromHandle(handle);
                acquire(c);
                c.ready = 1;
                while (c.release === 0)
                    $.agent.sleep(1);
                drop();
                for (let i = 0; i < 30; i++) {
                    gcDeep((i % 9) * 15);
                    $.agent.sleep(1);
                }
                c.released = 1;
            } catch (e) { $.agent.report('agent error: ' + e); }
            $.agent.leaving();
        });
    `;
    startAgents(1, source);
    $.agent.broadcast(sharedStructHandle(ctl, 1));
    waitUntil(() => ctl.ready === 1, "the agent to hold the cycle");
    ctl.cycle = undefined;
    for (let i = 0; i < 40; i++) {
        gcDeep((i % 9) * 15);
        collectSharedCycles();
        $.agent.sleep(2);
    }
    assert(liveSharedBlocks() >= blocks + 1 + 5, "the cycle is held by the agent and is not collected: " + liveSharedBlocks());
    ctl.release = 1;
    waitUntil(() => ctl.released === 1, "the agent to let go");
    // The control block itself stays, because the agent still has a wrapper of it.
    settle(() => liveSharedBlocks() <= blocks + 1, "the cycle once the agent let go");
}

// 8. The collector runs by itself once enough candidates have gathered.
{
    const blocks = liveSharedBlocks();
    (function make() {
        for (let i = 0; i < 20000; i++) {
            const a = new Node();
            const b = new Node();
            a.next = b;
            b.next = a;
            if (i % 2000 === 0)
                gc();
        }
    })();
    let ok = false;
    for (let i = 0; i < 600 && !ok; i++) {
        gcDeep((i % 9) * 15);
        $.agent.sleep(20);
        ok = liveSharedBlocks() <= blocks + 500;
    }
    assert(ok, "the background collector reclaims cycles without being asked: " + (liveSharedBlocks() - blocks) + " blocks left");
    print("# background: " + (liveSharedBlocks() - blocks) + " blocks left of 40000");
}

// 9. Agents rewrite the edges of a pool of nodes (and replace nodes, which leaves cyclic garbage behind) while
// this agent collects. Nothing may crash or read torn, and the garbage goes once the pool is dropped.
{
    const blocks = liveSharedBlocks();
    const POOL = 200, AGENTS = 3, ITERATIONS = 20000;
    const pool = new SharedArray(POOL + 1);
    for (let i = 0; i < POOL; i++) {
        const node = new Node();
        node.id = i;
        node.text = "node " + i;
        pool[i] = node;
    }
    pool[POOL] = 0;
    const source = `
        shared struct Node { next; other; text; id; }
        function gcDeep(n) {
            if (n > 0) {
                const pad = [n, n + 1, n + 2, n + 3];
                gcDeep(n - 1);
                return pad.length;
            }
            gc();
        }
        function check(node) {
            if (node === undefined || node === null) throw new Error("a pool entry is missing");
            const text = node.text;
            if (typeof text !== "string" || !(text.startsWith("node ") || text.startsWith("fresh ")))
                throw new Error("a torn read: " + text);
        }
        function work(handle) {
            const pool = sharedStructFromHandle(handle);
            const random = () => (Math.random() * ${POOL}) | 0;
            for (let i = 0; i < ${ITERATIONS}; i++) {
                const a = pool[random()];
                const b = pool[random()];
                check(a);
                check(b);
                a.next = b;
                if (i % 4 === 0)
                    a.other = a;
                if (i % 8 === 0) {
                    const fresh = new Node();
                    fresh.id = i;
                    fresh.text = "fresh " + i;
                    fresh.next = pool[random()];
                    fresh.other = fresh;
                    pool[random()] = fresh;
                }
                if (i % 4000 === 0)
                    gcDeep(i % 90);
            }
        }
        // Counts out through the last element of the pool, once the wrappers of the work are gone.
        function countOut(handle) {
            const pool = sharedStructFromHandle(handle);
            for (;;) {
                const current = Atomics.load(pool, "${POOL}");
                if (Atomics.compareExchange(pool, "${POOL}", current, current + 1) === current)
                    break;
            }
        }
        $.agent.receiveBroadcast(function (handle) {
            try {
                work(handle);
                for (let i = 0; i < 30; i++) {
                    gcDeep((i % 9) * 15);
                    $.agent.sleep(1);
                }
                countOut(handle);
            } catch (e) { $.agent.report('agent error: ' + e); }
            $.agent.leaving();
        });
    `;
    startAgents(AGENTS, source);
    // Each agent turns the handle into a wrapper twice: one reference for the work and one for counting out.
    $.agent.broadcast(sharedArrayHandle(pool, 2 * AGENTS));
    let reclaimed = 0, reads = 0;
    waitUntil(() => {
        reclaimed += collectSharedCycles();
        for (let k = 0; k < 20; k++) {
            const node = pool[(Math.random() * POOL) | 0];
            assert(typeof node.text === "string" && (node.text.startsWith("node ") || node.text.startsWith("fresh ")), "a torn read: " + node.text);
            const next = node.next;
            if (next !== undefined)
                assert(typeof next.id === "number", "a torn read of a neighbour");
            reads++;
        }
        return Atomics.load(pool, String(POOL)) >= AGENTS;
    }, "the agents to finish");
    print("# stress: " + reads + " reads, " + reclaimed + " blocks reclaimed while the agents ran");
    for (let i = 0; i < POOL; i++)
        pool[i] = undefined;
    // The pool and a few wrappers that an agent may still hold stay; leaked cycles would be thousands.
    settle(() => liveSharedBlocks() <= blocks + 1000, "the garbage of the stress");
    print("# stress: " + (liveSharedBlocks() - blocks) + " blocks above the start at the end");
}
