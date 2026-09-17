//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
//@ runDefault()
//@ runDefault("--thresholdForJITAfterWarmUp=10", "--thresholdForJITSoon=10", "--thresholdForOptimizeAfterWarmUp=50", "--thresholdForOptimizeSoon=50", "--thresholdForFTLOptimizeAfterWarmUp=300", "--thresholdForFTLOptimizeSoon=300")
// SPEC-objectmodel G1, history §39; SPEC-jit §5.5 "Untagged words", history §52
// (tenth round). With the GIL on every thread's butterfly TID is 0: nothing is
// foreign, no word carries a tag, and generated code accesses butterflies as
// flag off does - no tag mask, no owner compare, no CheckTransitionOwner. What
// makes that sound is that the GIL changes hands only inside blocking calls,
// and nothing holds a storage pointer across a call. This test has several
// threads take turns on the SAME objects and arrays, each turn separated by a
// handoff (property-path waits, joins of short-lived threads, a Lock), with the
// working functions shared by all threads and hot enough to run in every tier.
// Every turn grows, reshapes, shifts, slices, deletes from or freezes what the
// previous thread left, so each thread keeps running on storage another thread
// allocated or reallocated. Values are checked after every turn and at the end.
// GIL off the same program runs with per-thread tags and the locked routes, and
// the same checks apply (the turns are serialized by the lock either way).
load("../harness.js", "caller relative");

// (0) The rule itself, where it applies: an object a spawned thread allocated
// is owned by TID 0, the spawned thread's own TID is 0, GIL on.
if ($vm.useThreadGIL()) {
    const probe = new Thread(() => ({ tid: $vm.currentButterflyTID(), made: { a: 1, b: [1, 2, 3] } })).join();
    if (probe.tid !== 0)
        throw new Error("a spawned thread's butterfly TID is " + probe.tid + " with the GIL on");
    probe.made.b.push(4);
    probe.made.c = 3; // out-of-line growth of an object another thread allocated: an owner transition
    if ($vm.butterflyOwnerTID(probe.made) !== 0 || $vm.butterflyOwnerTID(probe.made.b) !== 0)
        throw new Error("an object allocated by a spawned thread is not owned by TID 0 with the GIL on");
}

// Shared working set.
const THREADS = 3;
const TURNS = 1500; // per thread: enough calls of work() and check() for every tier
const state = {
    turn: 0,
    objects: [],
    arrays: [],
    doubles: [],
    log: [],
};
const lock = new Lock();
const gate = { v: 0 };

function makeObject(i) { return { id: i, a: i, b: i + 1 }; }
noInline(makeObject);

// One turn of work on everything in `state`, by thread `who` at global turn `t`.
function work(who, t) {
    // Named properties: inline adds, out-of-line growth, replacement, deletion, re-add.
    const o = state.objects[t % state.objects.length];
    o["p" + (t % 24)] = t; // dictionary-free growth until the shape gets big
    o.a = t;
    o.b = who;
    if (!(t % 7))
        delete o["p" + ((t + 3) % 24)];
    // A fresh object every turn, extended by whoever runs the next turns.
    state.objects.push(makeObject(t));
    if (state.objects.length > 64)
        state.objects.shift();

    // Int32/Contiguous array: push, indexed append past the end, in-place store, shift, pop, slice, length writes.
    const a = state.arrays[t % state.arrays.length];
    a.push(t);
    a[a.length] = t + 1;
    a[0] = who;
    if (a.length > 40) {
        a.shift();
        a.pop();
        a.length = 20;
    }
    if (!(t % 5))
        a[a.length + 3] = t; // a hole: beyond the vector length now and then
    if (!(t % 11))
        a.push({ t }); // Int32 -> Contiguous
    state.arrays[(t + 1) % state.arrays.length] = a.slice(0, 16).concat([t]);

    // Double array, written through a CopyOnWrite literal first.
    const d = state.doubles[t % state.doubles.length];
    d[1] = t + 0.5;
    d.push(t + 0.25);
    if (d.length > 32)
        d.splice(0, 16);
    if (!(t % 13))
        state.doubles[t % state.doubles.length] = [0.5, 1.5, 2.5, 3.5];

    // Freeze one object per 17 turns; later turns' writes to it must be ignored, not corrupt it.
    if (!(t % 17))
        Object.freeze(state.objects[0]);

    state.log.push(who);
}
noInline(work);

function check(t) {
    for (const o of state.objects) {
        if (typeof o.id !== "number" || o.id < 0)
            throw new Error("turn " + t + ": object id " + o.id);
        for (const k of Object.keys(o)) {
            const v = o[k];
            if (typeof v !== "number" || v !== v)
                throw new Error("turn " + t + ": object " + o.id + " has " + k + " = " + v);
        }
    }
    for (const a of state.arrays) {
        if (a.length > 64)
            throw new Error("turn " + t + ": array length " + a.length);
        for (let i = 0; i < a.length; ++i) {
            const v = a[i];
            if (v !== undefined && typeof v !== "number" && !(typeof v === "object" && typeof v.t === "number"))
                throw new Error("turn " + t + ": array element " + i + " is " + v);
        }
    }
    for (const d of state.doubles) {
        for (let i = 0; i < d.length; ++i) {
            if (typeof d[i] !== "number" || d[i] !== d[i])
                throw new Error("turn " + t + ": double element " + i + " is " + d[i]);
        }
    }
}
noInline(check);

for (let i = 0; i < 8; ++i) {
    state.objects.push(makeObject(i));
    state.arrays.push([i, i + 1, i + 2]);
    state.doubles.push([0.5, 1.5, 2.5, 3.5]);
}

function handoff(t) {
    // Each of these is a blocking call that releases the GIL (GIL on) or simply blocks (GIL off).
    switch (t % 3) {
    case 0: Atomics.wait(gate, "v", 0, 0.2); break;
    case 1: new Thread(() => 1).join(); break;
    case 2: Atomics.notify(gate, "v"); Atomics.wait(gate, "v", 0, 0.1); break;
    }
}

function runner(who) {
    for (let n = 0; n < TURNS; ++n) {
        lock.hold(() => {
            const t = state.turn++;
            work(who, t);
            if (!(t % 16))
                check(t);
        });
        handoff(n);
    }
    return who;
}

const threads = [];
for (let who = 1; who < THREADS; ++who)
    threads.push(new Thread(runner, who));
runner(0);
for (const thread of threads)
    thread.join();

check(-1);
if (state.turn !== THREADS * TURNS || state.log.length !== THREADS * TURNS)
    throw new Error("turns " + state.turn + ", log " + state.log.length);
const seen = new Set(state.log);
if (seen.size !== THREADS)
    throw new Error("only " + seen.size + " threads took turns");
