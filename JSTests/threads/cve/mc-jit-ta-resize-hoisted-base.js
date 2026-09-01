//@ requireOptions("--useJSThreads=1")
// mc-jit-ta-resize-hoisted-base.js — MC-JIT surface S4 (docs/threads/cve/
// map-MC-JIT.md): typed-array fast paths' cached/hoisted {base, length}
// pairs vs concurrent detach / transfer / shrink / grow.
//
// EXECUTE POST-UNGIL under ASAN (this is one of the U28 amplifier arms owed
// by UNGIL-HANDOUT annex N6). Green-by-construction under the phase-1 GIL.
//
// Mechanism: every tier's TA fast path loads length, bounds-checks, then
// loads base; DFG/FTL additionally hoist vector/length out of loops. The
// annex N6 design makes every torn pair safe: detach publishes length=0 but
// quarantines the mapping to the next heap stop; shrink defers the physical
// free to the stop; grow is base-immutable (commit pages, then release-
// publish length); transfer = copy + detach. A build WITHOUT the quarantine
// (the landed code frees on the detaching/resizing thread) lets a reader
// holding {oldLen, oldBase} dereference a released mapping — ASAN UAF/OOB.
//
// Oracle: no crash; reads return only values the writers ever stored (or 0
// from fresh pages); post-detach accesses throw or return undefined per
// spec, never garbage.
load("../harness.js", "caller relative");

const MAX = 1 << 20;        // 1 MiB max for resizable buffers
const SMALL = 1 << 12;
const ROUNDS = 300;
const gate = { go: 0, started: 0, stop: 0 };

const READERS = 3;
const box = { buf: null, view: null, epoch: 0 };

function freshResizable() {
    const buf = new ArrayBuffer(SMALL, { maxByteLength: MAX });
    const view = new Uint32Array(buf);
    for (let i = 0; i < view.length; ++i)
        view[i] = 0x41410000 | (i & 0xffff);
    box.buf = buf;
    box.view = view;
    box.epoch++;
}
freshResizable();

const readers = spawnN(READERS, which => {
    Atomics.add(gate, "started", 1);
    while (Atomics.load(gate, "go") === 0)
        Atomics.wait(gate, "go", 0, 100);

    // Hot compiled loop: hoistable length + base, poll at the back edge.
    // Sums and domain-checks every element it can see.
    function sweep(v, seed) {
        let acc = 0;
        for (let i = 0; i < v.length; ++i) {
            const x = v[i];
            // Domain: writer patterns (0x4141xxxx / 0x5252xxxx), zero-fill
            // from fresh grow pages, or undefined (detached / OOB read on a
            // shrunk view). Anything else is heap garbage from a dead
            // mapping.
            if (x !== undefined && x !== 0
                && (x >>> 16) !== 0x4141 && (x >>> 16) !== 0x5252)
                throw new Error("reader " + which + " saw garbage @" + i
                    + ": 0x" + (x >>> 0).toString(16) + " seed=" + seed);
            acc = (acc + (x | 0)) | 0;
        }
        return acc;
    }
    noInline(sweep);

    // Writer flavor on odd readers: stale-base WRITES into a quarantined /
    // shrunk mapping are the UAF-write arm.
    function spray(v, seed) {
        for (let i = 0; i < v.length; ++i)
            v[i] = 0x52520000 | ((i + seed) & 0xffff);
    }
    noInline(spray);

    let sweeps = 0;
    while (!Atomics.load(gate, "stop")) {
        const v = box.view;
        if (!v) continue;
        try {
            if (which & 1)
                spray(v, sweeps);
            else
                sweep(v, sweeps);
        } catch (e) {
            if (!(e instanceof TypeError))   // detached-view TypeError is fine
                throw e;
        }
        sweeps++;
    }
    return sweeps > 0 ? "swept" : "idle";
});

waitUntil(() => Atomics.load(gate, "started") === READERS);
Atomics.store(gate, "go", 1);
Atomics.notify(gate, "go", Infinity);

// Main: the annex-N6 falsifier storm — grow / shrink / re-grow / transfer /
// detach churn on the buffer the compiled reader loops are sweeping.
for (let r = 0; r < ROUNDS; ++r) {
    const buf = box.buf;
    const mode = r % 5;
    try {
        switch (mode) {
        case 0:  // GROW: base-immutable arm — commit then publish length.
            buf.resize(Math.min(MAX, buf.byteLength * 2));
            break;
        case 1:  // SHRINK: deferred-free arm — tail must stay readable-safe.
            buf.resize(SMALL);
            break;
        case 2:  // re-grow after shrink: consumes/cancels pending tail entries.
            buf.resize(Math.min(MAX, SMALL * 8));
            break;
        case 3:  // TRANSFER: copy + detach arm; source mapping quarantined.
            box.buf = buf.transfer(buf.byteLength);
            box.view = new Uint32Array(box.buf);
            break;
        case 4:  // DETACH-equivalent + replacement buffer for the next wave.
            buf.transfer(0);              // detaches; old mapping quarantined
            freshResizable();
            break;
        }
    } catch (e) {
        // resize/transfer on an already-detached buffer between cases.
        if (!(e instanceof TypeError)) throw e;
    }
    // Keep view in sync with surviving buffer (stale views on readers'
    // stacks are exactly the point — do NOT synchronize them).
    if (mode <= 2 && !box.buf.detached)
        box.view = new Uint32Array(box.buf);
}

Atomics.store(gate, "stop", 1);
for (const r of readers)
    shouldBe(r.join(), "swept");
print("mc-jit-ta-resize-hoisted-base: PASS");
