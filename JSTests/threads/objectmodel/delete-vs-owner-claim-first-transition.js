//@ requireOptions("--useJSThreads=1")
// SPEC-objectmodel §4.3 / E4-C, seventh landing round (F33). Once a shape's
// thread-local sets are dead, the object's OWNER keeps transitioning it
// without the cell lock, claim-first (CAS StructureID -> nuked, write,
// publish), and every cell-locked writer is excluded from that leg only
// through the lane: it must treat a header that moved as a lost claim and
// RESTART. The delete leg of the sixth round still asserted "only volatile
// header bits move under the cell lock" and aborted when the owner's claim
// landed between its ID check and its header CAS (mirror harness,
// stress/delete-by-val-ftl.js, 3 in 100). Here the main thread creates
// objects and adds properties to them (owner, lock-free claim-first once the
// shape's sets are fired) while a second thread deletes a property from the
// same objects (foreign, cell-locked). Every object must end with `a` gone or
// present-and-1, `b`..`e` as written, and nobody may crash. GIL on / flag off
// run the same program (no claims there).
load("../harness.js", "caller relative");

const N = 20000;
const objs = new Array(N);
const box = { i: -1, stop: 0 };

const deleter = new Thread(() => {
    let deleted = 0, seen = -1;
    while (!Atomics.load(box, "stop")) {
        const i = Atomics.load(box, "i");
        if (i === seen) continue;
        seen = i;
        const o = objs[i];
        if (o && delete o.a) deleted++;
    }
    return deleted;
});

let sum = 0;
for (let i = 0; i < N; ++i) {
    const o = { a: 1 };          // owner: this thread
    objs[i] = o;
    Atomics.store(box, "i", i);  // hand it to the deleter...
    o.b = i; o.c = i + 1;        // ...while the owner keeps transitioning it (E4 / E4-C claim-first)
    Object.defineProperty(o, "d", { value: 2, writable: true, enumerable: true, configurable: true });
    o.e = 3;
    sum += o.b + o.c + o.d + o.e;
}
Atomics.store(box, "stop", 1);
const deleted = deleter.join();

let bad = 0;
for (let i = 0; i < N; ++i) {
    const o = objs[i];
    if (!(o.a === undefined || o.a === 1) || o.b !== i || o.c !== i + 1 || o.d !== 2 || o.e !== 3) bad++;
    if (o.a === undefined && Object.prototype.hasOwnProperty.call(o, "a")) bad++; // deleted means gone, not undefined-valued
}
if (sum !== (() => { let s = 0; for (let i = 0; i < N; ++i) s += i + i + 1 + 5; return s; })()) throw new Error("owner adds lost: " + sum);
if (bad) throw new Error(bad + " objects inconsistent after owner adds raced foreign deletes (" + deleted + " deletes)");
print("PASS");
