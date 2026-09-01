//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// Flag-on, putByIndex on a dense (Int32/Double/Contiguous) array routes through
// putIndexConcurrent before the generic beyond-vector path. An index far past
// the vector (indexIsSufficientlyBeyondLengthForSparseMap: i >= 1000 and
// i > vectorLength) must reach the generic path and take ArrayStorage plus a
// sparse map, exactly as flag-off does, instead of hole-filling dense storage
// out to the index. Each case uses its own literal site so the array
// allocation profile of one case cannot pick the layout of the next.
load("../harness.js", "caller relative");

{
    const a = [];
    a[5000] = 1;
    shouldBe($vm.indexingMode(a), "ArrayWithArrayStorage", "a[5000] on an empty array");
    shouldBe(a[5000], 1);
    shouldBe(a.length, 5001);
    shouldBe(a[4999], undefined);
}

{
    const a = [];
    a[99999] = 2.5;
    shouldBe($vm.indexingMode(a), "ArrayWithArrayStorage", "a[99999] on an empty array");
    shouldBe(a[99999], 2.5);
    shouldBe(a.length, 100000);
}

{
    const a = [1, 2, 3];
    a[1003] = "s";
    shouldBe($vm.indexingMode(a), "ArrayWithArrayStorage", "a[1003] on a three-element Int32 array");
    shouldBe(a[1003], "s");
    shouldBe(a[2], 3);
    shouldBe(a.length, 1004);
}

{
    const a = [0.5, 1.5];
    a[20000] = 3.5;
    shouldBe($vm.indexingMode(a), "ArrayWithArrayStorage", "a[20000] on a Double array");
    shouldBe(a[20000], 3.5);
    shouldBe(a[0], 0.5);
}

// Below the sparse threshold the dense layout stays dense.
{
    const a = [];
    a[999] = 1;
    shouldBe($vm.indexingMode(a), "ArrayWithInt32", "a[999] on an empty array");
    shouldBe(a.length, 1000);
}

// push never trips the sparse test: the index equals the public length, which
// never exceeds the vector length.
{
    const a = [];
    for (let i = 0; i < 1200; ++i)
        a.push(i);
    shouldBe($vm.indexingMode(a), "ArrayWithInt32", "1200 pushes");
    shouldBe(a[1199], 1199);
}
