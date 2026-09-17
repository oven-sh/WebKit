//@ requireOptions("--useJSThreads=1")
//@ runDefault()
//@ runDefault("--thresholdForJITAfterWarmUp=10", "--thresholdForJITSoon=10", "--thresholdForOptimizeAfterWarmUp=50", "--thresholdForOptimizeSoon=50", "--thresholdForFTLOptimizeAfterWarmUp=300", "--thresholdForFTLOptimizeSoon=300")
// SPEC-jit history §52 (tenth round). A property key built by concatenation
// (o[prefix + name]) is atomized through a per-site cache in DFG and FTL code:
// two quick entries probed inline, a map behind them in a C++ operation. Flag
// on, the inline probe was off and the operation took the cache's lock twice
// per miss, both against a second mutator inside the same call. With the GIL
// there is none, so GIL on both are main's code again. What that relies on:
// the GIL changes hands only inside blocking calls, and neither the probe nor
// the operation contains one. This test has three threads take turns at the
// SAME hot sites, a handoff between turns, with key sets chosen to walk a site
// through every state of its cache - the two quick entries, the map, the
// overflow to megamorphic - while other threads' turns interleave. Every read
// must return the value stored under exactly that key. GIL off the same program
// runs the locked operation and the same checks apply.
load("../harness.js", "caller relative");

const THREADS = 3;
const TURNS = 600; // per thread
const lock = new Lock();
const gate = { v: 0 };

// One object per site family; every value encodes its key.
const small = {};  // 2 keys: stays in the quick entries
const medium = {}; // 40 keys: spills into the map
const large = {};  // 600 keys: overflows the cache's capacity (megamorphic)
function fill(o, prefix, n) {
    for (let i = 0; i < n; ++i)
        o[prefix + i] = prefix + i + "!";
}
fill(small, "s_", 2);
fill(medium, "m_", 40);
fill(large, "l_", 600);

// The sites. Each function has its own caches; noInline keeps them one site each.
function readSmall(i) { return small["s_" + (i & 1)]; }
function readMedium(i) { return medium["m_" + (i % 40)]; }
function readLarge(i) { return large["l_" + (i % 600)]; }
function readTwoPart(o, a, b) { return o[a + b]; } // variable + variable
function writeThenRead(o, prefix, i, who) {
    const k = prefix + "w" + (i % 16);
    o[k] = k + "!" + who;
    return o[prefix + "w" + (i % 16)];
}
noInline(readSmall);
noInline(readMedium);
noInline(readLarge);
noInline(readTwoPart);
noInline(writeThenRead);

function turn(who, t) {
    for (let j = 0; j < 40; ++j) {
        const i = t * 40 + j;
        let v = readSmall(i);
        if (v !== "s_" + (i & 1) + "!")
            throw new Error("turn " + t + ": small key " + (i & 1) + " read " + v);
        v = readMedium(i);
        if (v !== "m_" + (i % 40) + "!")
            throw new Error("turn " + t + ": medium key " + (i % 40) + " read " + v);
        v = readLarge(i);
        if (v !== "l_" + (i % 600) + "!")
            throw new Error("turn " + t + ": large key " + (i % 600) + " read " + v);
        v = readTwoPart(medium, "m_", String(i % 40));
        if (v !== "m_" + (i % 40) + "!")
            throw new Error("turn " + t + ": two-part key " + (i % 40) + " read " + v);
        v = writeThenRead(medium, "x_", i, who);
        if (v !== "x_w" + (i % 16) + "!" + who)
            throw new Error("turn " + t + ": wrote as " + who + ", read " + v);
    }
}
noInline(turn);

function handoff(n) {
    switch (n % 3) {
    case 0: Atomics.wait(gate, "v", 0, 0.2); break;
    case 1: new Thread(() => 1).join(); break;
    case 2: Atomics.notify(gate, "v"); Atomics.wait(gate, "v", 0, 0.1); break;
    }
}

const state = { turn: 0 };
function runner(who) {
    for (let n = 0; n < TURNS; ++n) {
        lock.hold(() => turn(who, state.turn++));
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
if (state.turn !== THREADS * TURNS)
    throw new Error("turns " + state.turn);
