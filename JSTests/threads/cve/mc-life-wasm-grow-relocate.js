//@ requireOptions("--useJSThreads=1")
// MC-LIFE S6 (docs/threads/cve/map-MC-LIFE.md): relocating wasm Memory grow
// vs spawned typed-array readers — the documented OPEN DEPENDENCY in
// ArrayBufferContents::refreshAfterWasmMemoryGrow (ArrayBuffer.cpp): the
// stale-mapping quarantine half is landed, but the heap-stop conduction in
// Wasm::Memory::grow's relocating (BoundsChecking) arm is not. Until it
// lands, GIL-off a reader can pair a post-grow length with the pre-grow base
// and run off the end of the old (alive-but-short) mapping.
//
// SUSCEPTIBILITY TEST, expected to FAIL (crash/ASAN report) GIL-off until
// the N6 arm 4 stop conduction lands; passes under the GIL and must pass
// post-fix. Spawned threads perform ONLY typed-array accesses (SPEC-api
// refuses spawned wasm EXECUTION; views over a main-created Memory are plain
// TA accesses). Amplifier-ready: the hot reader loop is the torn-pair window.
load("../harness.js", "caller relative");

// FIXME(U-T13/MC-LIFE-S6): this premise-skip self-retires when the GIL-off
// wasm refusal is lifted (relocating-grow stop conduction lands); the guard
// below then never fires and the test runs at full strength.
// Wasm is deliberately refused GIL-off (U-T13: 'JSC: disabling useWasm under
// GIL-off...') until the N6 arm 4 stop conduction this test targets actually
// lands. That refusal is the accepted engine behavior, not a failure here:
// report the runner-recognized premise-skip marker (Tools/threads/run-tests.sh
// counts it as SKIP, never PASS) and exit 0.
if (typeof WebAssembly === "undefined") {
    print("THREADS-PREMISE-SKIP: WebAssembly is unavailable in the effective"
        + " configuration (deliberate U-T13 GIL-off wasm refusal); this"
        + " susceptibility test cannot run meaningfully without it.");
    quit();
}

const THREADS = 4;
const PAGE = 64 * 1024;
const SENTINEL = 0x7A7A7A7A;

// No `maximum` => no ceiling reservation is guaranteed => grow may RELOCATE
// the backing store (the BoundsChecking-without-VA arm).
const mem = new WebAssembly.Memory({ initial: 1 });

function stampedView() {
    const view = new Int32Array(mem.buffer);
    view.fill(SENTINEL);
    return view;
}

let view = stampedView();

const stop = new Int32Array(new SharedArrayBuffer(4));
const started = new Int32Array(new SharedArrayBuffer(4));

// Readers: hammer the CURRENT buffer's view hot enough to tier up, and keep
// re-reading a possibly-stale view captured around grows. After a grow the
// old buffer is detached (length 0): every read must be SENTINEL or
// undefined. A junk value or a crash is the susceptibility witness.
const readers = spawnN(THREADS, () => {
    Atomics.add(started, 0, 1);
    let checksum = 0;
    while (Atomics.load(stop, 0) === 0) {
        const v = view; // racy capture of the latest published view
        const n = v.length;
        for (let i = 0; i < n; i += 16) {
            const x = v[i];
            if (x !== SENTINEL && x !== undefined)
                throw new Error("reader saw corrupt word after relocate: " + x);
            checksum ^= x | 0;
        }
        // Also walk a fresh view of the current buffer (post-grow base).
        const fresh = new Int32Array(mem.buffer);
        const m = fresh.length;
        for (let i = 0; i < m; i += 1024) {
            const x = fresh[i];
            if (x !== SENTINEL && x !== 0 && x !== undefined)
                throw new Error("fresh view saw corrupt word: " + x);
        }
    }
    return checksum;
});

// Wait until every reader is hot.
while (Atomics.load(started, 0) < THREADS)
    sleepMs(1);

// Grower: relocate repeatedly while readers are mid-loop. Each grow detaches
// the previous buffer and (absent stop conduction) republishes base+length
// without quiescing the readers.
for (let g = 0; g < 24; ++g) {
    try {
        mem.grow(1);
    } catch (e) {
        break; // OOM-bounded; what ran is the test
    }
    view = stampedView(); // publish a view over the post-grow mapping
}

Atomics.store(stop, 0, 1);
joinAll(readers);
