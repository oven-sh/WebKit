// + WeakRefs only (no deref loop)
const weakRefs = [];
const K = 128;
function makeGarbage() {
    const ps = [];
    for (let i = 0; i < K; ++i) {
        const ta = new Int32Array(new SharedArrayBuffer(8));
        weakRefs.push(new WeakRef(ta));
        const r = Atomics.waitAsync(ta, 0, 0, 60000);
        ps.push(r.value);
        Atomics.notify(ta, 0, Infinity);
    }
    return Promise.all(ps);
}
makeGarbage().then(async values => {
    for (let turn = 0; turn < 200; ++turn) { gc(); await Promise.resolve(); }
    print("PASS-I");
});
