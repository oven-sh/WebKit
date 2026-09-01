// SPEC-vmstate task 9 (I13/I4): deterministic per-thread-execution-state
// workload, shared by the behavior-identity tests. The SAME digest must come
// out of every flag configuration (flags off; --useVMLite=1 single-threaded;
// --useJSThreads=1 which implies all three §3 flags via M_opts2 R2) and out
// of every thread that runs it — Phase A VMLites are inert carriers, so
// nothing here may observe them.
//
// Exercises every VMLite group's VM-side counterpart:
//   Group 2 (exception/unwind): throw/catch through real frames.
//   Group 3 (stack bookkeeping): unbounded recursion -> RangeError.
//   Group 4 (regexp, lazy regexp stack): global-regexp exec loop.
//   Group 6 (microtasks): Promise jobs + drainMicrotasks() ordering
//     (Phase A: VM::queueMicrotask/drainMicrotasks NOT rerouted, §6.1.1).
//   W1/W2 light touch: fresh property names (atomization) + fresh shapes
//     (Structure allocation) in the accumulation loop.
//
// Everything is deterministic: no Date, no Math.random, no GC observation.
// The caller must have loaded ../resources/assert.js (for shouldBe).

const VMSTATE_WORKLOAD_EXPECTED_DIGEST = "te50|range|re60:180|mt132|acc25750";

function runVMStateWorkload() {
    const pieces = [];

    // Group 2: exception state. Each catch must see exactly the thrown
    // TypeError; m_exception/callFrameForCatch/target*ForThrow round-trip.
    function thrower(depth) {
        if (!depth)
            throw new TypeError("vmstate-workload");
        return thrower(depth - 1);
    }
    let typeErrors = 0;
    for (let i = 0; i < 50; ++i) {
        try {
            thrower(i % 7);
        } catch (e) {
            if (e instanceof TypeError)
                ++typeErrors;
        }
    }
    shouldBe(typeErrors, 50, "workload: exception round-trips");
    pieces.push("te" + typeErrors);

    // Group 3: stack limit. Unbounded recursion must throw RangeError (the
    // stack-overflow check), never crash, and the frame must stay usable.
    function deep(n) {
        return deep(n + 1) | 0;
    }
    let limitOutcome = "nolimit";
    try {
        deep(0);
    } catch (e) {
        limitOutcome = e instanceof RangeError ? "range" : "other";
    }
    shouldBe(limitOutcome, "range", "workload: stack limit");
    pieces.push(limitOutcome);

    // Group 4: regexp execution (uses the lazily-allocated regexp stack).
    // Per "aacabcaaabbc" unit: matches "aac"(2+0), "abc"(1+1), "aaabbc"(3+2)
    // => 3 matches, capture-length sum 9; 20 units => 60 and 180.
    const re = /(a+)(b+)?c/g;
    const s = "aacabcaaabbc".repeat(20);
    let m;
    let count = 0;
    let sum = 0;
    while ((m = re.exec(s))) {
        ++count;
        sum += m[1].length + (m[2] ? m[2].length : 0);
    }
    shouldBe(count, 60, "workload: regexp match count");
    shouldBe(sum, 180, "workload: regexp capture sum");
    pieces.push("re" + count + ":" + sum);

    // Group 6: microtask ordering. Phase A drains the VM's queue exactly as
    // today: FIFO across independent chains, continuations re-enqueued.
    const order = [];
    Promise.resolve(1).then(v => order.push(v)).then(() => order.push(2));
    Promise.reject(new Error("vmstate")).catch(() => order.push(3));
    drainMicrotasks();
    shouldBe(order.join(""), "132", "workload: microtask ordering");
    pieces.push("mt" + order.join(""));

    // W1/W2 light touch: fresh property names atomize; fresh shapes allocate
    // Structures. sum_{r=0..99} sum_{p=0..4} (r+p) = 5*4950 + 100*10 = 25750.
    let acc = 0;
    for (let r = 0; r < 100; ++r) {
        const o = {};
        for (let p = 0; p < 5; ++p)
            o["k" + r + "_" + p] = r + p;
        for (const k in o)
            acc += o[k];
    }
    shouldBe(acc, 25750, "workload: structure/atom churn accumulation");
    pieces.push("acc" + acc);

    return pieces.join("|");
}
