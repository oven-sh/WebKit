// SPEC-objectmodel I22: flag-OFF single-threaded no-change suite.
//
// Runs with NO options: useJSThreads defaults to false, so per I22/E3 every
// layout and observable behavior must be identical to today (flat tags zero,
// mask a no-op, no quarantine split, no TTL watchpoint behavior change).
// This file pins the observable semantics the object model could plausibly
// perturb: transition order, delete + slot reuse, dictionary flattening,
// array shapes (Int32/Double/Contiguous/CoW/ArrayStorage), and enumeration
// order. Its twin i03-single-threaded-flag-on.js runs the SAME checks with
// --useJSThreads=1 and no Thread ever spawned; both must stay green forever.
load("../resources/assert.js", "caller relative");

// PREMISE SELF-CHECK: this is a DEFAULT-configuration (flag-off) witness.
// jsc applies JSC_<option> environment variables in Options::initialize(),
// before the (empty) argv this file runs with, so an ambient export like
// JSC_useJSThreads=1 (e.g. a GIL-off rung that pins its whole configuration
// via env) silently inverts the premise: the "Thread must not leak
// flag-off" assert below would then FAIL without any engine regression. We
// deliberately do NOT pin --useJSThreads=0 on the header: that would
// convert this default-config witness into an explicit-value test (and a
// future default flip would make its green vacuous). Instead, query the
// EFFECTIVE option at runtime via the unconditional shell builtin
// jscOptions() — same pattern as heap-option-off.js — and emit the
// THREADS-PREMISE-SKIP marker recognized by Tools/threads/run-tests.sh
// (counted as SKIP, never PASS or FAIL), so an env/premise contradiction
// surfaces as an actionable skip at the rung definition. The flag-ON
// behavior of these same checks is covered by the twin
// i03-single-threaded-flag-on.js, which pins --useJSThreads=1 explicitly.
if (typeof jscOptions === "function" && jscOptions().useJSThreads) {
    print("THREADS-PREMISE-SKIP: useJSThreads is ON in the effective configuration"
        + " (ambient JSC_* env?); i03-single-threaded-no-change.js is a"
        + " default-configuration (flag-off) witness per SPEC-objectmodel"
        + " I22/E3 and cannot run meaningfully under it. The flag-on twin"
        + " i03-single-threaded-flag-on.js carries the same checks.");
    quit();
}

// Flag-off there is no Thread API (the GIL stub is flag-gated).
shouldBe(typeof Thread, "undefined", "Thread must not leak flag-off");

// --- Property transitions, enumeration order, inline -> out-of-line ---
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

// --- Delete, re-add, same-name overwrite ---
{
    const o = { a: 1, b: 2, c: 3 };
    for (let i = 0; i < 30; ++i)
        o["x" + i] = "v" + i;
    delete o.x7;
    shouldBeFalse("x7" in o);
    shouldBe(o.x7, undefined);
    o.x7 = "readded";
    shouldBe(o.x7, "readded");
    shouldBe(Object.keys(o).length, 33);
    o.a = "overwritten";
    shouldBe(o.a, "overwritten");
}

// --- Dictionary mode via delete storm, then continued use ---
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

// --- Array shapes ---
{
    const int32 = [1, 2, 3];
    int32.push(4);
    shouldBe(int32.length, 4);
    shouldBe(int32[3], 4);

    const dbl = [0.5, 1.5];
    dbl[2] = 2.5;
    shouldBe(dbl[2], 2.5);
    dbl[3] = "string"; // Double -> Contiguous
    shouldBe(dbl[3], "string");
    shouldBe(dbl[0], 0.5);

    const cow = [10, 20, 30]; // CoW literal
    const cow2 = [10, 20, 30];
    cow[0] = 11; // materializes
    shouldBe(cow[0], 11);
    shouldBe(cow2[0], 10); // sibling untouched

    const grown = [];
    for (let i = 0; i < 1000; ++i)
        grown[i] = i * 2;
    shouldBe(grown.length, 1000);
    shouldBe(grown[999], 1998);

    const sparse = [];
    sparse[100000] = "far"; // ArrayStorage
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

// --- length / holes / in-operator agreement ---
{
    const a = [0, , 2]; // hole at 1
    shouldBe(a.length, 3);
    shouldBeFalse(1 in a);
    shouldBe(a[1], undefined);
    a.length = 1;
    shouldBe(a.length, 1);
    shouldBeFalse(2 in a);
}

// --- GC stability of all of the above ---
{
    const keep = { o: { z: 9 }, a: [1.5, 2.5] };
    for (let i = 0; i < 20; ++i)
        keep.o["g" + i] = { nested: i };
    gc();
    for (let i = 0; i < 20; ++i)
        shouldBe(keep.o["g" + i].nested, i);
    shouldBe(keep.a[1], 2.5);
}
