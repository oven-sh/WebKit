// Arm-3 shape: per-cell waitAsync+notify, Promise.all, then gc() turns.
const K = 128;
const weakRefs = [];
function makeWaitedOnGarbage() {
    const ps = [];
    for (let i = 0; i < K; ++i) {
        const cell = { v: 0 };
        weakRefs.push(new WeakRef(cell));
        const r = Atomics.waitAsync(cell, "v", 0, 60000);
        if (r.async !== true) throw new Error("expected async");
        ps.push(r.value);
        if (Atomics.notify(cell, "v", Infinity) !== 1) throw new Error("notify");
        const ne = Atomics.waitAsync(cell, "v", 999);
        if (ne.async !== false || ne.value !== "not-equal") throw new Error("ne");
    }
    return Promise.all(ps);
}
makeWaitedOnGarbage().then(async values => {
    for (const v of values)
        if (v !== "ok") throw new Error("bad settle: " + v);
    let cleared = 0;
    for (let turn = 0; turn < 2000 && cleared < K / 2; ++turn) {
        gc();
        await Promise.resolve();
        cleared = 0;
        for (const wr of weakRefs) if (wr.deref() === undefined) cleared++;
    }
    print("minrepro2: cleared=" + cleared + "/" + K);
});
