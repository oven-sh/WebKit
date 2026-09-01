//@ requireOptions("--useJSThreads=1")
// MC-LIFE S2 (docs/threads/cve/map-MC-LIFE.md): SharedArrayBufferContents
// refcount balance under cross-thread wrapper/view churn.
//
// Mozilla bug 1352681 shape: an unbalanced ref on a shared backing store's
// counter (taken on one agent, never released because a later step failed)
// eventually frees the mapping under live views -> cross-thread UAF. Our
// Thread() boundary has no serialize step, so this test churns every
// JS-reachable path that copies the SharedArrayBufferContents RefPtr
// (view creation, slice, growable-SAB grow, shared wasm Memory buffers)
// from multiple threads under GC pressure, then verifies sentinel bytes.
// A premature final-deref (unbalanced decrement, or a future unbalanced
// increment paired with a "fix") surfaces as a UAF — deterministic under
// ASAN, amplifier-ready under TSAN.
//
// Racing arms are meaningful with --useThreadGIL=0 (post-ungil ladder);
// under the GIL the test still validates balance sequentially.
load("../harness.js", "caller relative");

const THREADS = 4;
const ITERS = 200;

// --- Arm 1: plain SAB — per-thread view churn + GC pressure ---
{
    const sab = new SharedArrayBuffer(256);
    new Int32Array(sab).fill(0x5EadBeef | 0);
    const sentinel = new Int32Array(sab)[0];

    joinAll(spawnN(THREADS, () => {
        for (let i = 0; i < ITERS; ++i) {
            // Each constructor/slice takes and drops contents refs.
            const a = new Int32Array(sab);
            const b = new Uint8Array(sab, 16, 64);
            const c = new DataView(sab);
            const copy = sab.slice(0, 64); // fresh contents, not shared refs
            if (a[0] !== sentinel)
                throw new Error("sentinel torn/lost in arm 1: " + a[0]);
            if (new Int32Array(copy)[0] !== sentinel)
                throw new Error("slice copy wrong in arm 1");
            // GC pressure so wrapper finalizers (deref sites) actually run
            // concurrently with other threads' ref sites.
            if ((i & 31) === 0) {
                let garbage = [];
                for (let j = 0; j < 64; ++j)
                    garbage.push(new ArrayBuffer(1024));
                garbage = null;
            }
            void b; void c;
        }
    }));

    if (new Int32Array(sab)[0] !== sentinel)
        throw new Error("sentinel lost after arm 1 churn");
}

// --- Arm 2: growable SAB — concurrent grow + view churn ---
// SharedArrayBufferContents::grow runs under the memory handle's lock and
// publishes m_sizeInBytes; every thread also creates/drops length-tracking
// views (ref churn) while lengths move.
if (typeof SharedArrayBuffer.prototype.grow === "function") {
    const gsab = new SharedArrayBuffer(64, { maxByteLength: 64 * 1024 });
    new Int32Array(gsab)[0] = 0x1234567;

    const threads = [];
    for (let t = 0; t < THREADS; ++t) {
        threads.push(new Thread(tid => {
            for (let i = 0; i < ITERS; ++i) {
                const view = new Int32Array(gsab); // length-tracking
                if (view[0] !== 0x1234567)
                    throw new Error("sentinel lost during grow churn");
                const want = Math.min(64 * 1024, gsab.byteLength + 64);
                try {
                    gsab.grow(want);
                } catch (e) {
                    // A racing larger grow can make this a no-op/throw per
                    // spec; only TypeError/RangeError are acceptable.
                    if (!(e instanceof TypeError) && !(e instanceof RangeError))
                        throw e;
                }
                if (gsab.byteLength < 64)
                    throw new Error("growable SAB shrank: " + gsab.byteLength);
            }
            return tid;
        }, t));
    }
    joinAll(threads);
    if (new Int32Array(gsab)[0] !== 0x1234567)
        throw new Error("sentinel lost after grow churn");
}

// --- Arm 3: shared wasm Memory — its buffer shares the same contents ---
// Spawned threads only touch views (plain TA accesses are allowed on spawned
// threads; wasm EXECUTION is not). Buffer re-fetch after grow churns the
// contents RefPtr from every thread.
if (typeof WebAssembly !== "undefined") {
    const mem = new WebAssembly.Memory({ initial: 1, maximum: 16, shared: true });
    new Int32Array(mem.buffer)[0] = 0x0BadF00d | 0;
    const sentinel = new Int32Array(mem.buffer)[0];

    const stop = new Int32Array(new SharedArrayBuffer(4));
    const readers = spawnN(THREADS, () => {
        let last = 0;
        while (Atomics.load(stop, 0) === 0) {
            const view = new Int32Array(mem.buffer); // fresh wrapper + ref
            if (view[0] !== sentinel)
                throw new Error("wasm shared sentinel lost: " + view[0]);
            last = view.length;
        }
        return last;
    });
    for (let g = 0; g < 8; ++g) {
        try { mem.grow(1); } catch (e) { break; }
    }
    Atomics.store(stop, 0, 1);
    joinAll(readers);
    if (new Int32Array(mem.buffer)[0] !== sentinel)
        throw new Error("wasm shared sentinel lost after growth");
}
