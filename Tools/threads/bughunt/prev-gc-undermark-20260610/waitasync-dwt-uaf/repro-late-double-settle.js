const ta = new Int32Array(new SharedArrayBuffer(8));
const r = Atomics.waitAsync(ta, 0, 0, 60000);
Atomics.notify(ta, 0, Infinity);
r.value.then(async v => {
    if (v !== "ok") throw new Error("bad " + v);
    for (let turn = 0; turn < 200; ++turn) { gc(); await Promise.resolve(); }
    print("variantF: PASS");
});
