//@ requireOptions("--useJSThreads=1")
// SPEC-objectmodel I22 twin: flag ON, strictly single-threaded.
//
// Same observable-semantics pins as i03-single-threaded-no-change.js, but
// with --useJSThreads=1 and NO Thread ever spawned. Everything stays
// main-thread TID 0 / SW 0, so per §2 ("TID 0 (main) => bit-identical to
// today") and I22 the results must match the flag-off twin exactly. Any
// divergence is an object-model regression on the single-threaded fast path
// (tag dispatch, N3 install stamping, quarantine gating, §9.5 accessors).
load("../resources/assert.js", "caller relative");

shouldBe(typeof Thread, "function", "GIL-stub Thread API expected flag-on");

{
    const o = {};
    for (let i = 0; i < 40; ++i)
        o["p" + i] = i;
    shouldBe(Object.keys(o).length, 40);
    shouldBe(Object.keys(o)[0], "p0");
    shouldBe(Object.keys(o)[39], "p39");
    for (let i = 0; i < 40; ++i)
        shouldBe(o["p" + i], i);
}

{
    const o = { a: 1, b: 2, c: 3 };
    for (let i = 0; i < 30; ++i)
        o["x" + i] = "v" + i;
    delete o.x7;
    shouldBeFalse("x7" in o);
    shouldBe(o.x7, undefined);
    o.x7 = "readded"; // flag-on: quarantined slot must NOT be reused pre-epoch,
    shouldBe(o.x7, "readded"); // but the VALUE semantics are unchanged (I18/I22)
    shouldBe(Object.keys(o).length, 33);
    o.a = "overwritten";
    shouldBe(o.a, "overwritten");
}

{
    const o = {};
    for (let i = 0; i < 64; ++i)
        o["d" + i] = i;
    for (let i = 0; i < 64; i += 2)
        delete o["d" + i];
    shouldBe(Object.keys(o).length, 32);
    for (let i = 1; i < 64; i += 2)
        shouldBe(o["d" + i], i);
    o.afterDict = true;
    shouldBeTrue(o.afterDict);
}

{
    const int32 = [1, 2, 3];
    int32.push(4);
    shouldBe(int32.length, 4);
    shouldBe(int32[3], 4);

    const dbl = [0.5, 1.5];
    dbl[2] = 2.5;
    shouldBe(dbl[2], 2.5);
    dbl[3] = "string";
    shouldBe(dbl[3], "string");
    shouldBe(dbl[0], 0.5);

    const cow = [10, 20, 30];
    const cow2 = [10, 20, 30];
    cow[0] = 11;
    shouldBe(cow[0], 11);
    shouldBe(cow2[0], 10);

    const grown = [];
    for (let i = 0; i < 1000; ++i)
        grown[i] = i * 2;
    shouldBe(grown.length, 1000);
    shouldBe(grown[999], 1998);

    const sparse = [];
    sparse[100000] = "far";
    sparse[3] = "near";
    shouldBe(sparse.length, 100001);
    shouldBe(sparse[100000], "far");
    shouldBe(sparse[3], "near");
    shouldBe(sparse[50], undefined);

    const shifty = [1, 2, 3, 4, 5];
    shouldBe(shifty.shift(), 1);
    shifty.unshift(0);
    shouldBe(shifty[0], 0);
    shouldBe(shifty.length, 5);
}

{
    const a = [0, , 2];
    shouldBe(a.length, 3);
    shouldBeFalse(1 in a);
    shouldBe(a[1], undefined);
    a.length = 1;
    shouldBe(a.length, 1);
    shouldBeFalse(2 in a);
}

{
    const keep = { o: { z: 9 }, a: [1.5, 2.5] };
    for (let i = 0; i < 20; ++i)
        keep.o["g" + i] = { nested: i };
    gc();
    for (let i = 0; i < 20; ++i)
        shouldBe(keep.o["g" + i].nested, i);
    shouldBe(keep.a[1], 2.5);
}
