//@ requireOptions("--useJSThreads=1")
// DFG/FTL inlined transitions flag-on (SPEC-jit §5.5 Transition, DFG form:
// CheckTransitionOwner + PutByOffset + PutStructure). The optimizing tiers
// compile addProps with the transition chain inlined once it is hot on the
// main thread. Then:
//  (1) a second thread runs the same compiled function on butterfly-LESS
//      objects it just made: the shapes belong to the main thread (N1), so the
//      owner check must fail, exit, and the add must complete through the slow
//      path (which fires the shape's thread-local sets);
//  (2) the second thread runs it on objects that already have out-of-line
//      storage it allocated itself: those are instance-owned by that thread,
//      so the inlined transition is legal there and must produce exact values;
//  (3) the second thread runs it on objects the MAIN thread made and still
//      uses: foreign instance, must go slow and stay exact on both sides;
//  (4) the main thread keeps running the (recompiled) function throughout.
// Any lost or misplaced property, or a crash, fails the test.
load("../harness.js", "caller relative");

function addProps(o, v) { o.a = v; o.b = v + 1; o.c = v + 2; return o; }
noInline(addProps);
function withStorage(seed) {
    // Nine properties: past the default inline capacity, so the object has a
    // butterfly (out-of-line storage) before addProps runs.
    return { p0: seed, p1: 0, p2: 0, p3: 0, p4: 0, p5: 0, p6: 0, p7: 0, p8: seed };
}
noInline(withStorage);

function check(o, v, who) {
    if (o.a !== v || o.b !== v + 1 || o.c !== v + 2)
        throw new Error(who + ": " + JSON.stringify(o) + " for " + v);
}

for (let i = 0; i < 100000; ++i) {
    check(addProps({}, i), i, "warm-inline");
    check(addProps(withStorage(i), i), i, "warm-ool");
}

const shared = [];
for (let i = 0; i < 2000; ++i)
    shared.push(i & 1 ? withStorage(i) : { q: i });

const t = new Thread((sharedObjs) => {
    let n = 0;
    for (let i = 0; i < 50000; ++i) {
        const o = addProps({}, i * 3);            // (1)
        check(o, i * 3, "thread-inline");
        const p = addProps(withStorage(i), i);   // (2)
        check(p, i, "thread-ool");
        ++n;
    }
    for (let i = 0; i < sharedObjs.length; ++i) { // (3)
        addProps(sharedObjs[i], -i);
        check(sharedObjs[i], -i, "thread-shared");
        ++n;
    }
    return n;
}, shared);

for (let i = 0; i < 100000; ++i)                  // (4)
    check(addProps(i & 1 ? {} : withStorage(i), i), i, "main-during");

shouldBe(t.join(), 50000 + shared.length, "thread iterations");
for (let i = 0; i < shared.length; ++i) {
    check(shared[i], -i, "main-after-shared");
    shouldBe(i & 1 ? shared[i].p8 : shared[i].q, i, "pre-existing property " + i);
}
