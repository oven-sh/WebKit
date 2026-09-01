//@ requireOptions("--useJSThreads=1")
// richards-like: control-flow + property heavy, LOW allocation.
//
// Each thread runs its OWN miniature OS-scheduler simulation in the spirit of
// the Richards benchmark: a fixed set of tasks with priorities and states, a
// fixed pool of packets circulated between task queues (packets are REUSED,
// never reallocated — after warm-up the steady-state loop allocates nothing),
// and a dispatch loop that is all branches, polymorphic property access, and
// linked-list surgery. NO data is shared between threads.
//
// This is one of the suite's "non-allocating" workloads: with no GC pressure
// there is no excusable serial component, so scaling-gate.sh --gate holds it
// to the strict 2.8x@4 / 4.5x@8 thresholds. A miss here means the mutator
// hot path itself serializes (lock in property access / IC paths / dispatch),
// not the collector.
//
// Work size targets roughly 1s per thread on a release build at scale 1 (the gate's sizing; standalone corpus runs default to a fractional CORPUS_DEFAULT_SCALE - see harness.js);
// fixed iteration count, no blocking ops.
load("./harness.js", "caller relative");

function richardsWorkload() {
    const STEPS = Math.round(25000000 * scalingWorkScale());

    const KIND_IDLE = 0;
    const KIND_WORKER = 1;
    const KIND_DEVICE_A = 2;
    const KIND_DEVICE_B = 3;
    const KIND_HANDLER = 4;

    function Packet(id) {
        this.link = null;
        this.id = id;
        this.kind = KIND_WORKER;
        this.a1 = 0;
        this.a2 = [0, 0, 0, 0];
        this.hops = 0;
    }

    function Task(id, kind, priority) {
        this.id = id;
        this.kind = kind;
        this.priority = priority;
        this.queue = null;   // head of packet list
        this.held = false;
        this.suspended = false;
        this.scratch = 0;
        this.state = 0;
    }

    function enqueue(task, packet) {
        packet.link = null;
        packet.hops++;
        if (task.queue === null) {
            task.queue = packet;
            return;
        }
        let tail = task.queue;
        while (tail.link !== null)
            tail = tail.link;
        tail.link = packet;
    }

    function dequeue(task) {
        const packet = task.queue;
        if (packet !== null) {
            task.queue = packet.link;
            packet.link = null;
        }
        return packet;
    }

    // Fixed task set, fixed packet pool (10 packets). Both live for the whole
    // run; the steady-state loop only mutates fields and relinks packets.
    const tasks = [
        new Task(0, KIND_IDLE, 0),
        new Task(1, KIND_WORKER, 2),
        new Task(2, KIND_DEVICE_A, 3),
        new Task(3, KIND_DEVICE_B, 3),
        new Task(4, KIND_HANDLER, 1),
    ];
    for (let i = 0; i < 10; ++i)
        enqueue(tasks[1 + (i % 4)], new Packet(i));

    // Thread-local deterministic bit source for the idle task.
    let seed = 0x1f2e3d4c | 0;
    function nextBits() {
        seed ^= seed << 13;
        seed ^= seed >>> 17;
        seed ^= seed << 5;
        seed |= 0;
        return seed >>> 0;
    }

    let processed = 0;
    let idleTicks = 0;
    let holds = 0;
    let checksum = 0;

    for (let step = 0; step < STEPS; ++step) {
        // Pick the highest-priority runnable task with work (or the idle
        // task). Linear scan over 5 tasks: branchy, property-heavy.
        let best = tasks[0];
        for (let i = 1; i < tasks.length; ++i) {
            const t = tasks[i];
            if (t.held || t.suspended || t.queue === null)
                continue;
            if (best.kind === KIND_IDLE || t.priority > best.priority)
                best = t;
        }

        switch (best.kind) {
        case KIND_IDLE: {
            idleTicks++;
            // Release one held/suspended task per idle tick, round-robin,
            // steered by deterministic bits.
            const pick = tasks[1 + (nextBits() & 3)];
            if (pick.held) {
                pick.held = false;
                holds--;
            } else if (pick.suspended)
                pick.suspended = false;
            break;
        }
        case KIND_WORKER: {
            const packet = dequeue(best);
            packet.a1 = (packet.a1 + 1) | 0;
            for (let j = 0; j < packet.a2.length; ++j)
                packet.a2[j] = (packet.a2[j] + packet.a1 + j) & 0xffff;
            packet.kind = (packet.id & 1) === 0 ? KIND_DEVICE_A : KIND_DEVICE_B;
            enqueue(tasks[(packet.id & 1) === 0 ? 2 : 3], packet);
            processed++;
            break;
        }
        case KIND_DEVICE_A:
        case KIND_DEVICE_B: {
            const packet = dequeue(best);
            best.scratch = (best.scratch + packet.a2[packet.id & 3]) & 0xffffff;
            // Devices occasionally hold themselves (cleared by idle).
            if ((best.scratch & 31) === 7 && !best.held) {
                best.held = true;
                holds++;
            }
            packet.kind = KIND_HANDLER;
            enqueue(tasks[4], packet);
            processed++;
            break;
        }
        case KIND_HANDLER: {
            const packet = dequeue(best);
            checksum = (checksum + packet.a1 + packet.a2[0] + packet.hops) % 0x7fffffff;
            packet.kind = KIND_WORKER;
            // Handler occasionally suspends itself; worker resumes via idle.
            if ((checksum & 63) === 21)
                best.suspended = true;
            enqueue(tasks[1], packet);
            processed++;
            break;
        }
        }
    }

    let queued = 0;
    for (let i = 0; i < tasks.length; ++i) {
        let p = tasks[i].queue;
        while (p !== null) {
            queued++;
            p = p.link;
        }
    }
    shouldBe(queued, 10, "richards-like: packet pool conserved");

    return checksum + ":" + processed + ":" + idleTicks + ":" + tasks[2].scratch + ":" + tasks[3].scratch;
}

runScalingWorkload("richards-like", richardsWorkload);

// WOULD-FAIL-IF: the non-allocating mutator hot path serializes across
// threads — e.g. property access or inline-cache paths reacquire a global
// lock, Structure/transition watchpoint checks contend on shared state, or
// the dispatch loop's polymorphic access sites funnel through a serialized
// slow path under N mutators. Because this workload allocates nothing in
// steady state, GC cannot be blamed: speedup(4) < 2.8 or speedup(8) < 4.5
// under scaling-gate.sh --gate isolates regression to the execution engine
// itself — NOTE this half of the claim is live ONLY when the pinned --gate
// rung runs (see Tools/threads/INTEGRATE-scaling.md; default corpus runs
// check only the checksum half, plus the opt-in SCALING_SELF_TRIPWIRE in
// harness.js for gross re-serialization). Standalone, a cross-thread
// IC/metadata race that misdirects a property load shows up as a
// packet-conservation or checksum failure.
