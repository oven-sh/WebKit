//@ requireOptions("--useJSThreads=1")
// mc-jit-stale-base-grow-oob.js — MC-JIT surface S2(a) (docs/threads/cve/
// map-MC-JIT.md): stale CSE'd/hoisted flat butterfly base + refreshed
// publicLength after a foreign flat->segmented conversion + growth.
//
// DO NOT RUN against the phase-1 GIL'd build expecting a repro: the GIL
// serializes mutators, so this is green-by-construction there. Execute
// post-ungil, ideally under ASAN and Tools/threads/amplify.sh.
//
// Mechanism under test: DFGClobberize.h gives CheckTraps write(InternalState)
// only, so a DFG/FTL loop can carry a masked flat base across a safepoint
// poll while re-loading publicLength through it. The live publicLength
// (fragment 0 slot 0 low half) ALIASES the flat IndexingHeader and, after a
// foreign segmenting growth, can exceed the frozen flat-era vectorLength
// (OM I9b / C4). The in-tree contiguous in-bounds check
// (DFGSpeculativeJIT64.cpp:2784) compares publicLength only => indices in
// [frozenFlatVL, livePublicLength) pass the check and dereference past the
// flat allocation through the stale base. CVE-2019-5782 / CVE-2021-2388
// analog.
//
// Oracle (post-ungil): no crash (ASAN OOB read/write fires on a hit), and
// every value read out of the victim is in the written domain. The reader
// loop deliberately clobbers Butterfly_publicLength heap state (push on a
// DIFFERENT array) without clobbering JSObject_butterfly, to force the
// "stale base + fresh length" snapshot mix if the compiler allows it.
// Amplifier-ready: bounded rounds, all threads joined.
load("../harness.js", "caller relative");

const FLAT_LEN = 64;        // flat-era vectorLength territory
const GROW_TO = 4096;       // forces segmented growth well past flat VL
const ROUNDS = 50;
const SENTINEL = 7;

const gate = { go: 0, started: 0, done: 0 };

// The shared victims. Created on main, transitioned/converted by the spawned
// thread (a foreign thread for their butterfly TID tags), so their
// structures' TTL sets die early — putting main's compiled loops on the
// UNREGISTERED (non-elided, full-predicate) path the surface targets.
const victims = [];
for (let r = 0; r < ROUNDS; ++r) {
    const a = new Array(FLAT_LEN);
    for (let i = 0; i < FLAT_LEN; ++i)
        a[i] = SENTINEL;
    victims.push(a);
}
const shared = { victims, round: -1 };

// decoy: in-loop push target that clobbers the abstract Butterfly_publicLength
// heap (kills cached length defs) but not JSObject_butterfly (keeps a cached
// base def live, if the compiler is willing).
const decoy = [1];

// Hot read loop. Reads a[i] for i up to a.length re-loaded per iteration;
// the decoy push sits between the length use and the next iteration.
function readerSweep(a, sink) {
    let acc = 0;
    for (let i = 0; i < a.length; ++i) {  // length re-loaded through storage
        const v = a[i];
        if (v !== undefined && (typeof v !== "number" || (v | 0) < 0))
            throw new Error("read outside written domain at " + i + ": " + String(v));
        acc += (v | 0);
        decoy.push(i);                    // clobbers publicLength heap
        if (decoy.length > 256) decoy.length = 1;
        sink.x = acc;                     // keeps the loop body honest
    }
    return acc;
}
noInline(readerSweep);

// Hot write loop, same shape: stale base + refreshed length on the WRITE
// side is the OOB-write variant.
function writerSweep(a) {
    for (let i = 0; i < a.length; ++i) {
        a[i] = SENTINEL;                  // in-bounds contiguous put
        decoy.push(i);
        if (decoy.length > 256) decoy.length = 1;
    }
}
noInline(writerSweep);

const grower = spawnN(1, () => {
    Atomics.add(gate, "started", 1);
    while (Atomics.load(gate, "go") === 0)
        Atomics.wait(gate, "go", 0, 100);
    // Foreign thread: first a foreign write (fires F1/kills writeThreadLocal
    // the first time), then segmenting growth (foreign element resize = T2:
    // convert + new spine + setSegmentedPublicLength into the ALIASED header
    // slot). Each round targets the victim main is sweeping.
    for (let r = 0; r < ROUNDS; ++r) {
        while (Atomics.load(gate, "done") === 0 && shared.round < r) { /* spin */ }
        if (Atomics.load(gate, "done")) break;
        const a = shared.victims[r];
        a[0] = SENTINEL;                  // foreign write: SW flip / F1
        for (let j = FLAT_LEN; j < GROW_TO; ++j)
            a[j] = SENTINEL;              // foreign growth: segmented, publicLength climbs
    }
    return "grown";
})[0];

waitUntil(() => Atomics.load(gate, "started") === 1);

// Warm both sweeps to DFG/FTL on early victims before the race window.
const sink = { x: 0 };
for (let w = 0; w < 1e3; ++w) {
    readerSweep(victims[0], sink);
    writerSweep(victims[0]);
}

Atomics.store(gate, "go", 1);
Atomics.notify(gate, "go", Infinity);

for (let r = 1; r < ROUNDS; ++r) {
    shared.round = r;                     // release this round's victim to the grower
    // Sweep while the grower converts/grows the SAME array. A stale-base +
    // grown-publicLength snapshot reads/writes past the flat allocation.
    for (let k = 0; k < 20; ++k) {
        readerSweep(victims[r], sink);
        writerSweep(victims[r]);
    }
}
Atomics.store(gate, "done", 1);

shouldBe(grower.join(), "grown");

// Post-race integrity: every victim slot in [0, FLAT_LEN) is the sentinel
// (writerSweep last touched them) or a number; grown tails are sentinel or
// holes. Garbage here means a wild write landed.
for (let r = 0; r < ROUNDS; ++r) {
    const a = victims[r];
    for (let i = 0; i < a.length; ++i) {
        const v = a[i];
        if (v !== undefined && typeof v !== "number")
            throw new Error("victim " + r + "[" + i + "] corrupted: " + String(v));
    }
}
print("mc-jit-stale-base-grow-oob: PASS");
