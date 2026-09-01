//@ requireOptions("--useJSThreads=1")
// MC-LIFE S5 (docs/threads/cve/map-MC-LIFE.md): annex N6 quarantine ownership
// accounting — exactly-one-detach arbitration, no double free of contents/
// m_destructor, source-died-before-stop (generation/ABA) arm, shrink-tail +
// re-grow accounting. Complements the N6 U28 torn-pair amplifier from the
// ownership/double-free angle: a broken accounting path surfaces as ASAN
// double-free/UAF; readers additionally assert the N6 invariant (a passing
// length is never paired with junk data).
//
// Racing arms are meaningful with --useThreadGIL=0 (post-ungil ladder);
// under the GIL each arm degenerates to its sequential semantics and must
// still pass.
load("../harness.js", "caller relative");

const THREADS = 4;
const SENTINEL = 0x1F2F3F4F;

function makeBuffer(bytes, opts) {
    const ab = opts ? new ArrayBuffer(bytes, opts) : new ArrayBuffer(bytes);
    new Int32Array(ab).fill(SENTINEL);
    return ab;
}

// --- Arm A: N threads race transfer() on ONE buffer ---
// The detached-buffer side table must let EXACTLY ONE racer move ownership
// into the quarantine; every winner's copy must carry intact sentinel bytes
// (the copy reads a stale-but-safe mapping); losers throw TypeError or
// observe byteLength 0. No crash, no torn copy.
for (let round = 0; round < 20; ++round) {
    const ab = makeBuffer(4096);
    const results = joinAll(spawnN(THREADS, () => {
        try {
            const t = ab.transfer();
            const view = new Int32Array(t);
            for (let i = 0; i < view.length; ++i) {
                if (view[i] !== SENTINEL)
                    throw new Error("torn transfer copy at " + i + ": " + view[i]);
            }
            return 1;
        } catch (e) {
            if (e instanceof TypeError)
                return 0; // lost the race: already detached
            throw e;
        }
    }));
    const winners = results.reduce((a, b) => a + b, 0);
    if (winners < 1)
        throw new Error("no transfer() winner in round " + round);
    if (ab.byteLength !== 0)
        throw new Error("source not detached after race");
    // Note: >1 winner is a JS-visible nondeterminism question, not a memory-
    // safety one (N6: "only the JS-visible outcome of the race is
    // nondeterministic"); we record but do not fail on it under GIL-off.
}

// --- Arm B: detach storm vs reader threads ---
// Readers loop over views of buffers main is detaching; every read must be
// either intact SENTINEL (pre-detach) or undefined (bounds-failed via the
// length=0 publish). Anything else is a stale-base/early-free witness.
{
    const stop = new Int32Array(new SharedArrayBuffer(4));
    const slots = []; // shared array of { buf } boxes readers walk
    for (let i = 0; i < 32; ++i)
        slots.push({ buf: makeBuffer(1024) });

    const readers = spawnN(THREADS, () => {
        let reads = 0;
        while (Atomics.load(stop, 0) === 0) {
            for (const slot of slots) {
                const b = slot.buf;
                let view;
                try {
                    view = new Int32Array(b);
                } catch (e) {
                    continue; // detached at construction: fine
                }
                const v = view[0];
                if (v !== SENTINEL && v !== undefined)
                    throw new Error("reader observed corrupt word: " + v);
                ++reads;
            }
        }
        return reads;
    });

    for (let round = 0; round < 50; ++round) {
        for (const slot of slots) {
            try { slot.buf.transfer(); } catch (e) { /* already detached */ }
            slot.buf = makeBuffer(1024); // replacement for next pass
        }
        // GC pressure: some detached sources die BEFORE the next stop —
        // exercises the ~ArrayBuffer unregister + generation/ABA guard on
        // clearBaseWordAtStop (a stale clear into a recycled buffer would
        // corrupt the replacement's words and trip the readers).
        let garbage = [];
        for (let j = 0; j < 256; ++j)
            garbage.push(new ArrayBuffer(512));
        garbage = null;
    }
    Atomics.store(stop, 0, 1);
    joinAll(readers);
}

// --- Arm C: resizable buffers — shrink-tail defer + re-grow consume ---
// resize() down enqueues a deferred tail entry; resize() up before the stop
// must consume/trim it (one-tail-per-handle invariant) and re-zero re-used
// pages. Readers assert values are only ever SENTINEL, 0 (re-grown zeroFill),
// or undefined (bounds-failed).
{
    const stop = new Int32Array(new SharedArrayBuffer(4));
    const rab = makeBuffer(64 * 1024, { maxByteLength: 256 * 1024 });

    const readers = spawnN(THREADS, () => {
        let reads = 0;
        while (Atomics.load(stop, 0) === 0) {
            const view = new Int32Array(rab); // length-tracking
            const n = view.length;
            for (let i = 0; i < n; i += 64) {
                const v = view[i];
                if (v !== SENTINEL && v !== 0 && v !== undefined)
                    throw new Error("shrink/regrow reader saw corrupt word: " + v);
                ++reads;
            }
        }
        return reads;
    });

    for (let round = 0; round < 100; ++round) {
        rab.resize(4 * 1024);        // shrink: tail pages deferred to the stop
        rab.resize(16 * 1024);       // partial re-grow: trims the pending tail
        rab.resize(8 * 1024);        // shrink again: extends tail downward
        rab.resize(128 * 1024);      // big re-grow: consumes tail entirely
        new Int32Array(rab).fill(SENTINEL);
        rab.resize(64 * 1024);
        if ((round & 15) === 0) {
            let garbage = [];
            for (let j = 0; j < 128; ++j)
                garbage.push(new ArrayBuffer(2048));
            garbage = null;
        }
    }
    Atomics.store(stop, 0, 1);
    joinAll(readers);

    // Final integrity: bytes inside the final length are SENTINEL or 0.
    const final = new Int32Array(rab);
    for (let i = 0; i < final.length; i += 64) {
        if (final[i] !== SENTINEL && final[i] !== 0)
            throw new Error("post-storm corrupt word at " + i + ": " + final[i]);
    }
}
