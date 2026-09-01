//@ requireOptions("--useJSThreads=1", "--useThreadGIL=0", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1")
// MC-GROW/S4 + S8 SUSCEPTIBLE repro: fixed-length typed-array view racing
// detach (ArrayBuffer.prototype.transfer) hits the annex N6 torn-pair
// invariant in the GIL-off concurrent put_by_val/get_by_val TA fast path
// (trySetIndexQuicklyForTypedArrayViewConcurrent /
//  tryGetIndexQuicklyForTypedArrayViewConcurrent, runtime/JSObject.cpp:597-616).
//
// JSArrayBufferView::detachFromArrayBuffer() (JSArrayBufferView.cpp:263-270)
// stores m_length=0 then m_vector=null under cellLock; the concurrent fast
// path bounds-checks i < lengthRaw() then dereferences typedVector() with NO
// vector snapshot or null guard. Interleaving:
//   reader: load m_length -> 4096  (pre-detach)
//   writer: m_length=0; m_vector=null
//   reader: load m_vector -> null
//   reader: store *(null + i) -> SEGV / OOB-at-nullpage
//
// Governing invariant violated: SPEC-ungil annex N6 PRINCIPLE/INVARIANT —
// "a racing reader must NEVER pair a passing length with an unmapped-or-short
// base". The §10-stop quarantine keeps ArrayBuffer::m_data alive but the view's
// m_vector is cleared synchronously (FIXME(threads) in
// JSGenericTypedArrayViewInlines.h:870-875 acknowledges this); the JSObject.cpp
// concurrent fast path lacks the null-vector bail that
// JSGenericTypedArrayView::getIndexQuicklyAsNativeValue/setIndexQuicklyToNativeValue
// (JSGenericTypedArrayViewInlines.h:847-913) carry.
//
// Expected: SEGV / ASAN fault at near-null address while the hole is open;
// passes once the concurrent TA fast path snapshots the vector and bails on
// null (or m_vector clear is deferred to the heap §10 stop, per the FIXME).
load("../resources/assert.js", "caller relative");

if (typeof ArrayBuffer.prototype.transfer !== "function")
    throw new Error("transfer() unavailable; surface unreachable");

const READERS = 4;
const ROUNDS = 4000;
const SIZE = 4096;
const PATTERN = 0x5a;

const gate = new Int32Array(new SharedArrayBuffer(8));
const mailbox = { stop: false, view: null };

const readers = spawnN(READERS, () => {
    Atomics.add(gate, 0, 1);
    let sink = 0;
    while (!mailbox.stop) {
        const view = mailbox.view;
        if (!view)
            continue;
        // Tight inner storm: every iteration is a {len, base} torn-pair probe
        // through the JSObject.cpp concurrent TA fast path.
        for (let i = 0; i < 5000; ++i) {
            view[4095] = PATTERN;                // trySetIndexQuicklyConcurrent
            view[2048] = PATTERN;
            const v = view[4095];                // tryGetIndexQuicklyConcurrent
            if (!(v === undefined || v === 0 || v === PATTERN))
                throw new Error("S4: illegal observation 0x" + v.toString(16));
            sink += (v | 0);
        }
    }
    return sink;
});

// Spin (no Atomics.wait — TA-lane wait holds the JSLock per harness.js note)
// until all readers are running.
while (Atomics.load(gate, 0) < READERS) { }

for (let r = 0; r < ROUNDS; ++r) {
    const ab = new ArrayBuffer(SIZE);
    const v = new Uint8Array(ab);
    mailbox.view = v;
    ab.transfer(); // detach: m_length=0, m_vector=null on every incoming view
}

mailbox.stop = true;
joinAll(readers);
