//@ requireOptions("--useJSThreads=1", "--forceSegmentedButterflies=1", "--verifyConcurrentButterfly=1", "--thresholdForJITAfterWarmUp=10", "--thresholdForJITSoon=10")
// Native fast paths that read array elements must not decode a segmented
// butterfly word as flat storage. With forceSegmentedButterflies every array
// below is segmented once it grows, so each call reaches the fast path with a
// segmented word, and the results must match a flat run. No race is needed.
load("../harness.js", "caller relative");

// Grows past the initial vector, so the array is segmented.
function grown(values) {
    const array = [values[0]];
    for (let i = 0; i < 40; ++i)
        array.push(values[0]);
    array.length = 0;
    for (const value of values)
        array.push(value);
    return array;
}

// Array.prototype.concat with two or more arguments.
{
    const ints = grown([1, 2, 3]);
    const doubles = grown([0.5, 1.5]);
    const objects = grown(["a", { b: 1 }]);

    const intResult = ints.concat(grown([4, 5]), 6, grown([7]));
    shouldBe(intResult.join(","), "1,2,3,4,5,6,7");

    const doubleResult = ints.concat(doubles, 2.5);
    shouldBe(doubleResult.join(","), "1,2,3,0.5,1.5,2.5");

    const mixed = objects.concat(ints, "z");
    shouldBe(mixed.length, 6);
    shouldBe(mixed[1].b, 1);
    shouldBe(mixed[4], 3);
    shouldBe(mixed[5], "z");

    const fromThread = new Thread((a, b) => a.concat(b, [9, 10]).join(","), ints, doubles).join();
    shouldBe(fromThread, "1,2,3,0.5,1.5,9,10");
}

// String.raw over Int32, Double, and Contiguous raw arrays.
{
    shouldBe(String.raw({ raw: grown(["x", "y", "z"]) }, 1, 2), "x1y2z");
    shouldBe(String.raw({ raw: grown([10, 20, 30]) }, "-", "+"), "10-20+30");
    shouldBe(String.raw({ raw: grown([0.5, 1.5]) }, "|"), "0.5|1.5");
    const raw = grown(["p", "q"]);
    shouldBe(new Thread(r => String.raw({ raw: r }, "!"), raw).join(), "p!q");
}

// JSON.stringify of Contiguous and Int32 arrays, with and without a gap.
{
    const strings = grown(["s0", "s1", "s2"]);
    const ints = grown([1, 2, 3]);
    shouldBe(JSON.stringify(strings), '["s0","s1","s2"]');
    shouldBe(JSON.stringify(ints), "[1,2,3]");
    shouldBe(JSON.stringify({ strings, ints }), '{"strings":["s0","s1","s2"],"ints":[1,2,3]}');
    shouldBe(JSON.stringify(strings, null, 1), '[\n "s0",\n "s1",\n "s2"\n]');
    shouldBe(new Thread(a => JSON.stringify(a), strings).join(), '["s0","s1","s2"]');
}

// A JIT-compiled put_by_val_direct whose slow path profiles a segmented
// target. Array.from stores into whatever the constructor returns.
{
    let target;
    function Ctor() { return target; }
    for (let i = 0; i < 300; ++i) {
        target = grown([0, 1]);
        const result = Array.from.call(Ctor, [7, 8, 9]);
        shouldBe(result, target);
        shouldBe(target.join(","), "7,8,9");
    }
}
