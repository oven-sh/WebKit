//@ requireOptions("--useJSThreads=1")
// SPEC-vmstate W2 stress (I8/I9), transition-variety arm: drives the OTHER
// Structure-allocating paths the M7 checklist audits — delete (dictionary
// transitions), prototype change, preventExtensions/seal/freeze transitions,
// dictionary flattening (via Object.keys on a dictionary), and array
// indexing-type transitions (int32 -> double -> contiguous -> array storage)
// — concurrently from several threads.
//
// I8: every one of these allocating transitions must run under exactly one
// StructureAllocationLocker (fail-stop counter). I9: values must read back
// exactly; Object.isFrozen/isSealed/isExtensible must report the transition.
load("../resources/assert.js", "caller relative");

const THREADS = 4;
const ROUNDS = 60;

const threads = spawnN(THREADS, t => {
    let digest = 0;
    for (let r = 0; r < ROUNDS; ++r) {
        // Dictionary via delete: unique names force fresh transitions first.
        const o = {};
        for (let p = 0; p < 12; ++p)
            o["d" + t + "_" + r + "_" + p] = p;
        delete o["d" + t + "_" + r + "_3"];
        delete o["d" + t + "_" + r + "_7"];
        if (("d" + t + "_" + r + "_3") in o)
            throw new Error("delete lost");
        let names = Object.keys(o);
        if (names.length !== 10)
            throw new Error("dictionary shape wrong: " + names.length);
        for (const k of names)
            digest += o[k];

        // Prototype change transition.
        const proto = { ["proto" + t + "_" + r]: 100 + r };
        const child = { own: r };
        Object.setPrototypeOf(child, proto);
        digest += child["proto" + t + "_" + r] + child.own;

        // preventExtensions / seal / freeze transitions.
        const pe = { a: 1 };
        Object.preventExtensions(pe);
        if (Object.isExtensible(pe))
            throw new Error("preventExtensions transition lost");
        const sealed = Object.seal({ b: 2 });
        if (!Object.isSealed(sealed))
            throw new Error("seal transition lost");
        const frozen = Object.freeze({ c: 3 });
        if (!Object.isFrozen(frozen))
            throw new Error("freeze transition lost");
        try {
            "use strict";
            frozen.c = 99;
        } catch { }
        digest += pe.a + sealed.b + frozen.c; // frozen.c must still be 3.

        // Array indexing-type transitions.
        const arr = [];
        for (let i = 0; i < 16; ++i)
            arr.push(i);              // int32
        arr.push(0.5);                // -> double
        arr.push("s" + t + "_" + r);  // -> contiguous
        arr[40] = t;                  // hole -> array storage path
        if (arr.length !== 41)
            throw new Error("array transition lost length");
        digest += arr[15] + arr[16] + arr[40];
    }
    return digest;
});

const results = joinAll(threads);
// All threads compute the same value-shape except the thread-dependent bits;
// recompute expectations exactly.
for (let t = 0; t < THREADS; ++t) {
    let expected = 0;
    for (let r = 0; r < ROUNDS; ++r) {
        expected += (66 - 3 - 7);          // dictionary values 0..11 minus deleted 3,7
        expected += (100 + r) + r;         // prototype + own
        expected += 1 + 2 + 3;             // pe/sealed/frozen (frozen.c unchanged)
        expected += 15 + 0.5 + t;          // array reads
    }
    shouldBe(results[t], expected, "thread " + t + " digest");
}
