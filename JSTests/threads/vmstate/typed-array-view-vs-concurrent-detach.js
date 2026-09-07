//@ requireOptions("--useJSThreads=1")
// A view constructed on a buffer another thread is detaching (transfer())
// must come out either usable on live memory or neutered (length 0). GIL off,
// the constructor's detached check happens before the view registers with the
// buffer; a detach in between neutered only the views registered so far, and
// the late view kept a base pointer into the quarantined mapping, which the
// next stop frees: reads then returned freed memory (seen as a non-sentinel
// word). Readers here construct views in a tight loop and force collections so
// the quarantine retires while such a view is live.
load("../harness.js", "caller relative");

const SENTINEL = 0x1F2F3F4F;
const THREADS = 4;
function makeBuffer(bytes) { const ab = new ArrayBuffer(bytes); new Int32Array(ab).fill(SENTINEL); return ab; }

const stop = new Int32Array(new SharedArrayBuffer(4));
const slots = [];
for (let i = 0; i < 16; ++i) slots.push({ buf: makeBuffer(256) });

const readers = spawnN(THREADS, (k) => {
    let reads = 0, bad = null;
    while (Atomics.load(stop, 0) === 0 && !bad) {
        for (const slot of slots) {
            let view;
            try { view = new Int32Array(slot.buf); } catch (e) { continue; } // detached before construction
            // Give the quarantine a chance to retire between construction and use.
            if ((reads & 255) === 0) { const g = []; for (let j = 0; j < 64; ++j) g.push(new ArrayBuffer(64)); }
            const v = view[0];
            const w = view[view.length - 1];
            if ((v !== SENTINEL && v !== undefined) || (w !== SENTINEL && w !== undefined)) { bad = "reader " + k + " saw " + v + "/" + w + " len " + view.length; break; }
            ++reads;
        }
    }
    if (bad) throw new Error(bad);
    return reads;
});

for (let round = 0; round < 200; ++round) {
    for (const slot of slots) {
        try { slot.buf.transfer(); } catch (e) { }
        slot.buf = makeBuffer(256);
    }
    if ((round & 7) === 0) gc(); else edenGC();
}
Atomics.store(stop, 0, 1);
const total = joinAll(readers).reduce((a, b) => a + b, 0);
print("PASS");
