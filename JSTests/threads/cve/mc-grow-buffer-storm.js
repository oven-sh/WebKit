//@ requireOptions("--useJSThreads=1", "--useThreadGIL=0")
// MC-GROW susceptibility storm (docs/threads/cve/map-MC-GROW.md, surfaces
// S2/S3/S4/S5a/S8): resize/detach/transfer must never let a racing reader
// pair a passing length with an unmapped-or-short base (SPEC-ungil annex N6
// PRINCIPLE/INVARIANT).
//
// WRITTEN FOR THE POST-UNGIL EXECUTION PASS — do not run against a
// mid-bring-up tree. GIL-off only (the N6 arms are gilOffProcess-gated);
// under --useThreadGIL=1 every arm is serialized and the test is vacuous.
//
// Failure signals: process crash / ASAN fault (the real verdict carrier for
// this mechanism class) or an assertion below (a reader observed a value
// that no legal interleaving produces). The loops are deliberately hot so
// tiered-up TA fast paths (S8) execute, not just LLInt. Amplifier-ready:
// the AMPLIFIER.md hooks at the N6 choke points (detach, quarantine enqueue,
// length publish) widen the windows without changing this file.
load("../resources/assert.js", "caller relative");

const PATTERN8 = 0x5a;
const PATTERN32 = 0x5a5a5a5a;
const READERS = 3;
const HOT = 20000; // reader loop iterations per round; enough to tier up

// A reader observation is legal iff it is:
//   undefined  - index out of bounds at access time (shrink/detach won)
//   0          - never-written (or freshly grown/zero-filled) byte/element
//   PATTERN    - the writer's value
// Anything else is a torn/OOB read: stale base paired with a passing length.
function checkObserved(v, pattern, tag) {
    if (v === undefined || v === 0 || v === pattern)
        return;
    throw new Error(tag + ": illegal observation " + v
        + " (expected undefined, 0, or 0x" + pattern.toString(16) + ")");
}

// Readers poll mailbox.view every iteration so arms can swap buffers under
// them. They probe both ends of the CURRENT length: index length-1 was
// in-bounds at the moment length was loaded, so a correct engine must make
// the access safe (return a legal value or undefined) even if a resize /
// detach lands between the length load and the element access.
function spawnReaders(mailbox, pattern, tag) {
    return spawnN(READERS, () => {
        let sink = 0;
        while (!mailbox.stop) {
            const view = mailbox.view;
            if (!view)
                continue;
            for (let i = 0; i < HOT; ++i) {
                const len = view.length;
                if (!len)
                    continue;
                const last = view[len - 1];
                checkObserved(last, pattern, tag + "/last");
                const first = view[0];
                checkObserved(first, pattern, tag + "/first");
                const mid = view[(len >> 1)];
                checkObserved(mid, pattern, tag + "/mid");
                sink += (last | 0) + (first | 0) + (mid | 0);
            }
        }
        return sink;
    });
}

function spawnWriter(mailbox, pattern) {
    return new Thread(() => {
        while (!mailbox.stop) {
            const view = mailbox.view;
            if (!view)
                continue;
            for (let i = 0; i < HOT; ++i) {
                const len = view.length;
                if (!len)
                    continue;
                // In-bounds at length-load time; a resize racing this store
                // must either land it or make it a silent OOB no-op — never
                // a wild store.
                view[len - 1] = pattern;
                view[(len >> 1)] = pattern;
            }
        }
    });
}

function runArm(tag, pattern, mutate, rounds) {
    const mailbox = { stop: false, view: null };
    const readers = spawnReaders(mailbox, pattern, tag);
    const writer = spawnWriter(mailbox, pattern);
    for (let r = 0; r < rounds; ++r)
        mutate(mailbox, r);
    mailbox.stop = true;
    joinAll(readers);
    writer.join();
}

// ---- Arm S2: growable SharedArrayBuffer in-place grow ----
// Base immutable, commit-then-publish (ArrayBuffer.cpp:1436-1515). Readers
// racing grow may see {oldLen, base} or {newLen, base}; both in-bounds.
(function gsabGrowArm() {
    let probe = null;
    try {
        probe = new SharedArrayBuffer(8, { maxByteLength: 1 << 20 });
    } catch { return; } // growable SAB unsupported in this build
    if (typeof probe.grow !== "function")
        return;
    runArm("gsab-grow", PATTERN32, (mailbox, r) => {
        const gsab = new SharedArrayBuffer(16, { maxByteLength: 1 << 20 });
        mailbox.view = new Uint32Array(gsab); // length-tracking
        for (let size = 1 << 6; size <= (1 << 20); size <<= 1)
            gsab.grow(size);
        shouldBe(gsab.byteLength, 1 << 20);
    }, 8);
})();

// ---- Arm S3: resizable ArrayBuffer shrink / re-grow-after-shrink ----
// Shrink publishes the smaller length seq_cst and quarantines the tail
// pages to the next heap stop (resizeGILOff + deferShrinkTailGILOff); a
// reader's {oldLen, base} must land on still-committed pages. The gc()
// calls force quarantine retirement to interleave with the storm.
(function rabShrinkArm() {
    let probe = null;
    try {
        probe = new ArrayBuffer(8, { maxByteLength: 1 << 16 });
    } catch { return; } // resizable AB unsupported
    if (typeof probe.resize !== "function")
        return;
    const haveGC = typeof gc === "function";
    runArm("rab-shrink", PATTERN8, (mailbox, r) => {
        const rab = new ArrayBuffer(1 << 16, { maxByteLength: 1 << 16 });
        mailbox.view = new Uint8Array(rab); // length-tracking
        for (let i = 0; i < 40; ++i) {
            rab.resize(1 << 6);          // shrink: tail deferred, not decommitted
            rab.resize(1 << 16);         // re-grow: consumes the pending tail
            rab.resize((1 << 12) + 64);  // partial shrink (non-page-aligned length)
            rab.resize(1 << 16);
            if (haveGC && !(i & 7))
                gc();                    // retire quarantined tails mid-storm
        }
    }, 6);
})();

// ---- Arm S4: detach via transfer(), incl. resizable source + transferee resize ----
// transfer() is COPY + DETACH GIL-off (ArrayBuffer.cpp:925-1008 + detach
// :1012-1131): the source's length goes 0 seq_cst with the mapping
// quarantined; racing readers see undefined or stale-but-safe values.
(function detachTransferArm() {
    if (typeof ArrayBuffer.prototype.transfer !== "function")
        return;
    const haveGC = typeof gc === "function";
    const haveResizable = (() => {
        try { new ArrayBuffer(8, { maxByteLength: 64 }); return true; } catch { return false; }
    })();
    runArm("detach-transfer", PATTERN8, (mailbox, r) => {
        for (let i = 0; i < 50; ++i) {
            // Plain fixed-length source: transfer == detach storm.
            const ab = new ArrayBuffer(4096);
            const v = new Uint8Array(ab);
            v.fill(PATTERN8);
            mailbox.view = v;
            const moved = ab.transfer();
            shouldBe(ab.byteLength, 0);
            shouldBe(new Uint8Array(moved)[123], PATTERN8);

            if (haveResizable) {
                // Resizable source under reader storm; transferee then
                // resized up to maxByteLength (annex N6 r14 F2 arm), and
                // transfer to a LARGER size than byteLength.
                const rab = new ArrayBuffer(1024, { maxByteLength: 8192 });
                const rv = new Uint8Array(rab);
                rv.fill(PATTERN8);
                mailbox.view = rv;
                const big = rab.transfer(2048); // newByteLength > byteLength
                shouldBe(rab.byteLength, 0);
                if (typeof big.resize === "function")
                    big.resize(8192);
                shouldBe(new Uint8Array(big)[1000], PATTERN8);
                shouldBe(new Uint8Array(big)[1500], 0); // grown region zero
            }
        }
        // Let transferee + detached sources die pre-stop, then force a stop:
        // exercises the ~ArrayBuffer-between-detach-and-stop unregister path.
        mailbox.view = null;
        if (haveGC)
            gc();
    }, 4);
})();

// ---- Arm S5a: wasm Signaling / reserved-VA grow (in-place) ----
// Default fast memory: base immutable, pages committed before the larger
// length is published (refreshAfterWasmMemoryGrow gilOff branch). With a
// non-resizable buffer the old buffer detaches per grow instead — also a
// legal-arms-only outcome for the readers.
(function wasmSignalingGrowArm() {
    if (typeof WebAssembly === "undefined" || typeof WebAssembly.Memory !== "function")
        return;
    runArm("wasm-grow-inplace", PATTERN8, (mailbox, r) => {
        const mem = new WebAssembly.Memory({ initial: 1, maximum: 64 });
        let buf = null;
        if (typeof mem.toResizableBuffer === "function") {
            try { buf = mem.toResizableBuffer(); } catch { buf = mem.buffer; }
        } else
            buf = mem.buffer;
        mailbox.view = new Uint8Array(buf);
        for (let pages = 1; pages < 64; ++pages) {
            mem.grow(1);
            if (typeof mem.toResizableBuffer !== "function") {
                // Classic semantics: old buffer detached; rebind the readers.
                mailbox.view = new Uint8Array(mem.buffer);
            }
        }
        shouldBe(mem.buffer.byteLength >= 64 * 65536, true);
    }, 4);
})();
