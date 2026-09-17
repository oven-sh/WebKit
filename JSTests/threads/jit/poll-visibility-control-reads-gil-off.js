//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
//@ threadsRequireGILOff
// SPEC-jit I21, history §50 (tenth round). GIL off, an FTL loop's polls no
// longer make every heap read of the loop unhoistable; they keep only the
// reads that can decide the loop's control flow fresh. This test pins both
// halves of that rule and its memory-safety leg.
//
// (1) Every read that reaches a branch is still poll-bounded. A worker calls
//     call-free, allocation-free spin functions thousands of times each; the
//     main thread ends every spin with a plain store. The functions spin on a
//     plain named field, an array element, an array's length, a typed array
//     element, a closure variable and a global variable, through different
//     shapes of condition (loop condition, `if` + break, a value carried
//     through a local and a Phi, a value stored to another object and read
//     back, a cancel flag under `if` inside a counted loop). Calling them
//     over and over matters: a function that is called once and spins is only
//     ever entered through loop OSR entry, whose code has no pre-header to
//     hoist into, and would pass even with every read hoistable. After the
//     FTL has compiled a function for entry at its top, a read hoisted out of
//     its loop is performed once per call and the spin never ends (the
//     runner's timeout fails the test; a build whose FTL polls write no value
//     heaps at all hangs in the first function).
// (2) Data-only reads may be hoisted, and a typed array's base and length are
//     among them; the buffer's memory must never be used after it is gone. A
//     worker sums a Float64Array in a counted FTL loop (the float-mm shape)
//     while the main thread transfers the buffer away (which detaches the
//     view; GIL off the old mapping is quarantined and released at the next
//     collection's stop), forces collections, and allocates over the freed
//     range. Every sum must be one the view could legally produce: the full
//     sum while attached, or a sum of a prefix followed by zeros/undefined
//     coerced (NaN is accepted) after the detach - and nothing may crash
//     (Debug/ASan builds catch a read of released memory).
load("../harness.js", "caller relative");

// ---- (1) spins ----
const box = { stop: 0, a: 1, b: 2 };
const arr = [0, 5, 6];
const lenArr = [1, 2, 3];
const ta = new Int32Array(new SharedArrayBuffer(16));
let closureFlag = 0;
globalThis.globalFlag = 0;
const relay = { seen: 0 };
const BOUND = 2000000000; // seconds of iterations per call if the flag were never seen

// [spin, release (main thread, plain stores only), reset (worker, between rounds)]
const spins = [
    [function spinNamed() { let s = 0; while (!box.stop) s += box.a + box.b; return s; }, () => { box.stop = 1; }, () => { box.stop = 0; }],
    [function spinElement() { let s = 0; while (!arr[0]) s += arr[1] + arr[2]; return s; }, () => { arr[0] = 1; }, () => { arr[0] = 0; }],
    [function spinLength() { let s = 0; while (lenArr.length < 4) s += lenArr[1]; return s; }, () => { lenArr.push(4); }, () => { lenArr.length = 3; }],
    [function spinTyped() { let s = 0; while (!ta[0]) s += ta[1]; return s; }, () => { ta[0] = 1; }, () => { ta[0] = 0; }], // plain typed-array accesses, not Atomics
    [function spinClosure() { let s = 0; while (!closureFlag) s += 1; return s; }, () => { closureFlag = 1; }, () => { closureFlag = 0; }],
    [function spinGlobal() { let s = 0; while (!globalFlag) s += 1; return s; }, () => { globalThis.globalFlag = 1; }, () => { globalThis.globalFlag = 0; }],
    [function spinIfBreak() { let s = 0; for (;;) { s += box.a; if (box.stop) break; } return s; }, () => { box.stop = 1; }, () => { box.stop = 0; }],
    [function spinThroughLocal() { let s = 0; let seen = 0; do { seen = box.stop; s += box.b; } while (!seen); return s; }, () => { box.stop = 1; }, () => { box.stop = 0; }],
    [function spinThroughStore() { let s = 0; for (;;) { relay.seen = box.stop; s += box.a; if (relay.seen) break; } return s; }, () => { box.stop = 1; }, () => { box.stop = 0; relay.seen = 0; }],
    [function spinCounted() { let s = 0; for (let i = 0; i < BOUND; ++i) { s += i & 7; if (box.stop) return i; } return -1; }, () => { box.stop = 1; }, () => { box.stop = 0; }],
];
const ROUNDS = 4000;
const turn = new Int32Array(new SharedArrayBuffer(8)); // [0]: 2 * round + 1 = the worker is about to spin, 2 * round + 2 = it returned and reset

const spinner = new Thread(() => {
    let turnValue = 0;
    for (const [spin, release, reset] of spins) {
        for (let round = 0; round < ROUNDS; ++round) {
            Atomics.store(turn, 0, ++turnValue);
            const result = spin();
            if (typeof result !== "number" || result === -1)
                return spin.name + " returned " + result + " in round " + round;
            reset();
            Atomics.store(turn, 0, ++turnValue);
        }
    }
    return "ok";
});
{
    let turnValue = 0;
    let seed = 99;
    for (const [spin, release, reset] of spins) {
        for (let round = 0; round < ROUNDS; ++round) {
            ++turnValue;
            while (Atomics.load(turn, 0) < turnValue) { }
            // Let the spin run for a varying, short while so the loop itself gets hot.
            seed = (seed * 1103515245 + 12345) & 0x7fffffff;
            for (let wait = seed % 2000; wait > 0; --wait) { }
            release();
            ++turnValue;
            while (Atomics.load(turn, 0) < turnValue) { }
        }
    }
}
const spinVerdict = spinner.join();
if (spinVerdict !== "ok")
    throw new Error(spinVerdict);

// ---- (2) a typed-array kernel against a transfer of its buffer ----
const N = 4096;
function makeView() {
    const v = new Float64Array(N);
    for (let i = 0; i < N; ++i)
        v[i] = 1;
    return v;
}
const shared = { view: makeView(), go: 1, rounds: 0 };
function kernel(v, n) {
    let s = 0;
    for (let i = 0; i < n; ++i)
        s += v[i]; // v's vector and length are loop-invariant: hoisted GIL off since this round
    return s;
}
noInline(kernel);
for (let i = 0; i < 3000; ++i) {
    if (kernel(shared.view, N) !== N)
        throw new Error("warm-up sum");
}
const summer = new Thread(() => {
    let bad = null;
    let calls = 0;
    while (Atomics.load(shared, "go")) {
        const v = shared.view;
        const s = kernel(v, N);
        ++calls;
        // Attached: N. Detached during or before the call: reads are undefined from the detach on, so the sum is NaN
        // (or a finite prefix sum if the exit re-ran the rest generically and produced NaN anyway).
        if (!(s === N || Number.isNaN(s)))
            bad = bad || ("sum " + s + " after " + calls + " calls");
    }
    return bad || ("ok " + calls);
});
let junk = [];
for (let round = 0; round < 40; ++round) {
    sleepMs(5);
    const old = shared.view;
    shared.view = makeView();          // the summer picks the fresh view up on its next call
    old.buffer.transfer();             // detaches `old`; its mapping goes to the quarantine
    fullGC();                          // the collection's stop retires the quarantine
    for (let k = 0; k < 64; ++k)
        junk.push(new Float64Array(N).fill(7)); // reuse of the released range, if it was released into the allocator
    if (junk.length > 1024)
        junk = [];
    shared.rounds = round;
}
Atomics.store(shared, "go", 0);
const verdict = summer.join();
if (!String(verdict).startsWith("ok"))
    throw new Error("kernel saw " + verdict);
