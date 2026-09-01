//@ requireOptions("--useJSThreads=1")
// A thread that resumes from a park (here Thread.join) returns to bytecode
// whose linked calls run no slow path, so the stack limit the interpreter
// checks must already be this thread's own when the lock is re-acquired. If
// it were still the previous holder's, unbounded recursion on the C loop
// interpreter would run past the thread's own stack segment instead of
// throwing RangeError at the same depth as before the park.
load("../resources/assert.js", "caller relative");

let depth = 0;
function deep() {
    ++depth;
    deep();
}

function overflowDepth() {
    depth = 0;
    try {
        deep();
    } catch (e) {
        if (!(e instanceof RangeError))
            throw new Error("expected RangeError, got " + e);
        return depth;
    }
    throw new Error("recursion never hit the stack limit");
}

// Warm up first: every call site below is linked afterwards, which is the
// shape that reaches no republish site between the park and the overflow.
const before = overflowDepth();
shouldBeTrue(before > 100, "sane overflow depth before the park");

// The spawned thread overflows its own segment, so the limit left published
// when the joiner resumes is the spawned thread's.
const spawned = new Thread(() => {
    let spawnedDepth = 0;
    function spawnedDeep() {
        ++spawnedDepth;
        spawnedDeep();
    }
    try {
        spawnedDeep();
    } catch (e) {
        if (!(e instanceof RangeError))
            throw e;
        return spawnedDepth;
    }
    throw new Error("spawned thread never hit the stack limit");
});
shouldBeTrue(spawned.join() > 100, "sane overflow depth on the spawned thread");

// Running past the own segment into the spawned thread's (dead) one would
// roughly double the depth; tier-up between the two runs moves it far less.
const after = overflowDepth();
shouldBeTrue(after > 100, "sane overflow depth after the park");
shouldBeTrue(after <= before * 1.5, "overflow depth after the park must be bounded by this thread's own segment: " + before + " -> " + after);
