//@ requireOptions("--useJSThreads=1")
// MC-LIFE S11 (docs/threads/cve/map-MC-LIFE.md): backing-store allocator scope
// vs creator-thread mortality — the Node.js #28777 / V8 d8 cr/1215233004
// analog. A spawned Thread CREATES buffers (SAB, growable SAB, plain
// ArrayBuffer, resizable ArrayBuffer), stamps sentinels, returns them to main
// by reference (SPEC-api I2 identity), and EXITS. Main then churns views,
// detaches/resizes the survivors, and applies GC pressure across multiple
// stops with the creator's GCClient::Heap / VMLite already torn down.
//
// In-engine destructors are process-immortal (Gigacage::free closure captures
// nothing, ArrayBuffer.cpp:114; BufferMemoryManager is LazyNeverDestroyed,
// BufferMemoryHandle.cpp:199-202; the N6 quarantine is per-server-heap), so
// this MUST pass. A regression that ties contents/destructor/quarantine state
// to the creating client surfaces here as ASAN UAF / crash once the creator
// is gone. Every existing MC-LIFE arm creates on MAIN and shares to spawned;
// this test is the reverse direction.
//
// Racing arms are meaningful with --useThreadGIL=0 (post-ungil ladder);
// under the GIL the test still validates the sequential creator-dies path.
load("../harness.js", "caller relative");

const ROUNDS = 8;
const SENTINEL = 0x4C49_4645 | 0; // "LIFE"

function gcPressure(n) {
    let g = [];
    for (let j = 0; j < n; ++j)
        g.push(new ArrayBuffer(1024));
    g = null;
}

for (let round = 0; round < ROUNDS; ++round) {
    // --- Creator: a spawned thread allocates everything, then dies. ---
    const t = new Thread(() => {
        const sab = new SharedArrayBuffer(4096);
        new Int32Array(sab).fill(SENTINEL);

        let gsab = null;
        if (typeof SharedArrayBuffer.prototype.grow === "function") {
            gsab = new SharedArrayBuffer(1024, { maxByteLength: 64 * 1024 });
            new Int32Array(gsab).fill(SENTINEL);
        }

        const ab = new ArrayBuffer(4096);
        new Int32Array(ab).fill(SENTINEL);

        const rab = new ArrayBuffer(16 * 1024, { maxByteLength: 64 * 1024 });
        new Int32Array(rab).fill(SENTINEL);

        // Also detach one buffer FROM the creator so a quarantine entry is
        // enqueued by a client that is about to die (S5 generation/ABA arm,
        // creator-mortality direction).
        const doomed = new ArrayBuffer(2048);
        new Int32Array(doomed).fill(SENTINEL);
        const moved = doomed.transfer();
        if (new Int32Array(moved)[0] !== SENTINEL)
            throw new Error("creator-side transfer copy torn");

        return { sab, gsab, ab, rab, moved };
    });
    const { sab, gsab, ab, rab, moved } = t.join();
    // Creator thread has now fully exited: its GCClient::Heap / TLC / VMLite
    // are (or are about to be) torn down. Force at least one stop so the
    // creator's lastChanceToFinalize / DCT and the creator-enqueued
    // quarantine entry's retiring stop have both happened before we touch
    // the survivors.
    gcPressure(512);
    if (typeof gc === "function") gc();

    // --- Survivor checks: every backing store must outlive its creator. ---
    if (new Int32Array(sab)[0] !== SENTINEL)
        throw new Error("SAB sentinel lost after creator died (round " + round + ")");
    if (new Int32Array(ab)[0] !== SENTINEL)
        throw new Error("ArrayBuffer sentinel lost after creator died");
    if (new Int32Array(rab)[0] !== SENTINEL)
        throw new Error("resizable AB sentinel lost after creator died");
    if (new Int32Array(moved)[0] !== SENTINEL)
        throw new Error("creator-transferred buffer sentinel lost");

    // --- Churn: ref/deref + detach/resize on the orphans, with concurrent
    // sibling readers so the contents Ref/deref sites interleave with the
    // (now foreign-owned) buffers' accounting. ---
    const stop = new Int32Array(new SharedArrayBuffer(4));
    const readers = spawnN(3, () => {
        let n = 0;
        while (Atomics.load(stop, 0) === 0) {
            const v1 = new Int32Array(sab);
            if (v1[0] !== SENTINEL)
                throw new Error("sibling reader: SAB sentinel lost");
            const v2 = new Int32Array(ab);
            const x = v2[0];
            if (x !== SENTINEL && x !== undefined) // undefined once detached
                throw new Error("sibling reader: AB corrupt word " + x);
            const v3 = new Int32Array(rab);
            const y = v3[0];
            if (y !== SENTINEL && y !== 0 && y !== undefined)
                throw new Error("sibling reader: rab corrupt word " + y);
            ++n;
        }
        return n;
    });

    // Grow the orphaned growable SAB from a thread that did NOT create it.
    if (gsab !== null) {
        for (let i = 0; i < 8; ++i) {
            try { gsab.grow(Math.min(64 * 1024, gsab.byteLength + 1024)); }
            catch (e) {
                if (!(e instanceof TypeError) && !(e instanceof RangeError))
                    throw e;
            }
        }
        if (new Int32Array(gsab)[0] !== SENTINEL)
            throw new Error("growable SAB sentinel lost after foreign grow");
    }

    // Resize the orphaned resizable AB down/up (N6 shrink-tail accounting on
    // a buffer whose creator client is gone).
    rab.resize(4 * 1024);
    rab.resize(32 * 1024);
    new Int32Array(rab).fill(SENTINEL);

    // Detach the orphaned plain AB from main: the destructor closure that
    // eventually runs was BUILT on the dead creator; it must be capture-free.
    const copy = ab.transfer();
    if (new Int32Array(copy)[0] !== SENTINEL)
        throw new Error("foreign transfer of orphaned AB torn");
    if (ab.byteLength !== 0)
        throw new Error("orphaned AB not detached");

    gcPressure(256);
    if (typeof gc === "function") gc();

    Atomics.store(stop, 0, 1);
    joinAll(readers);

    // Final integrity after the post-creator stop(s).
    if (new Int32Array(sab)[0] !== SENTINEL)
        throw new Error("SAB sentinel lost at end of round " + round);
    if (new Int32Array(copy)[0] !== SENTINEL)
        throw new Error("transferred copy sentinel lost at end of round " + round);
}
