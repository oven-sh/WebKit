//@ requireOptions("--useJSThreads=1", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1", "--useDollarVM=1")
// SPEC-api 4.5: Atomics.store on an absent index at or past a dense array's
// vector length must grow the dense shape, exactly as GIL-on does. GIL-off
// the indexed Missing arm used to convert the receiver to ArrayStorage and
// park the element in a sparse map; from then on every Atomics op on ANY
// index of that array threw "Atomics property operations require a plain
// data property", while the same program GIL-on kept working. Single
// thread, deterministic: no race is needed to reach the Missing arm.
load("../harness.js", "caller relative");

function checkStaysDense(label, arr, index, value) {
    shouldBe(Atomics.store(arr, index, value), value, label + ": store returns v");
    shouldBe(arr.length, index + 1, label + ": length");
    shouldBe(arr[index], value, label + ": element");
    const mode = $vm.indexingMode(arr);
    shouldBeTrue(mode.indexOf("ArrayStorage") === -1, label + ": stays dense, got " + mode);
    // Every later Atomics op on the array keeps working.
    shouldBe(Atomics.load(arr, 0), arr[0], label + ": load(0)");
    shouldBe(Atomics.load(arr, index), value, label + ": load(index)");
    shouldBe(Atomics.store(arr, index, "again"), "again", label + ": second store");
    shouldBe(Atomics.load(arr, index), "again", label + ": load after second store");
    const first = arr[0];
    shouldBe(Atomics.compareExchange(arr, 0, first, first), first, label + ": CAS(0)");
}

// One past the end of each dense shape (CoW and writable words).
checkStaysDense("int32+int", [1, 2, 3], 3, 4);
checkStaysDense("int32+double", [1, 2, 3], 3, 4.5);
checkStaysDense("int32+string", [1, 2, 3], 3, "s");
checkStaysDense("double+double", [1.5, 2.5], 2, 3.5);
checkStaysDense("double+string", [1.5, 2.5], 2, "s");
checkStaysDense("contiguous", ["a", "b"], 2, "c");
{
    const writable = [1, 2, 3];
    writable[0] = 10; // leaves copy-on-write
    checkStaysDense("writable int32", writable, 3, 4);
}

// Further past the end, but still vector-worthy (below the sparse
// thresholds): grows densely too.
checkStaysDense("gap of 20", ["a", "b"], 22, "c");
checkStaysDense("gap of 500", [1, 2], 502, 3);

// Repeated appends through Atomics.store keep the array dense throughout.
{
    const arr = [0];
    for (let i = 1; i < 64; ++i)
        Atomics.store(arr, i, i);
    shouldBe(arr.length, 64);
    shouldBeTrue($vm.indexingMode(arr).indexOf("ArrayStorage") === -1, "64 appends stay dense, got " + $vm.indexingMode(arr));
    let sum = 0;
    for (let i = 0; i < 64; ++i)
        sum += Atomics.load(arr, i);
    shouldBe(sum, 63 * 64 / 2);
    shouldBe(Atomics.add(arr, 63, 1), 63);
    shouldBe(arr[63], 64);
}

// A plain object receiver with dense indexed storage behaves the same.
{
    const o = {};
    o[0] = "a";
    o[1] = "b";
    shouldBe(Atomics.store(o, 2, "c"), "c");
    shouldBe(Atomics.load(o, 0), "a");
    shouldBe(Atomics.load(o, 2), "c");
    shouldBeTrue($vm.indexingMode(o).indexOf("ArrayStorage") === -1, "object receiver stays dense, got " + $vm.indexingMode(o));
}
