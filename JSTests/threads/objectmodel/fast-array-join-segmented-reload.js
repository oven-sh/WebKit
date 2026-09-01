//@ requireOptions("--useJSThreads=1", "--verifyConcurrentButterfly=1")
// Array.prototype.join's Contiguous fast path runs user code (toString) for
// each element and then re-probes the array's butterfly word to decide whether
// the fast walk may continue. Flag-on that probe must go through the tagged
// word: a toString that pushes past vectorLength on a foreign-owned or
// shared-write array converts the word to segmented, and the flat-only
// butterfly() accessor would then trip its flatness check. Deterministic under
// the GIL: one thread runs the whole join.
load("../resources/assert.js", "caller relative");

function makeArray() {
    const arr = ["s0", "s1", "s2", "s3"];
    arr.push({
        toString() {
            for (let i = 0; i < 100; ++i)
                arr.push("x" + i);
            return "obj";
        }
    });
    arr.push("tail");
    return arr;
}

// Foreign caller: the spawned thread joins an array the main thread owns.
{
    const arr = makeArray();
    const result = new Thread(a => a.join(","), arr).join();
    shouldBe(result, "s0,s1,s2,s3,obj,tail");
    shouldBe(arr.length, 106);
    shouldBe(arr[105], "x99");
}

// Owner caller on a shared-write word: a foreign store sets SW, so the owner's
// later growth (from inside toString) goes segmented too.
{
    const arr = makeArray();
    new Thread(a => { a[0] = "S0"; }, arr).join();
    shouldBe(arr.join(","), "S0,s1,s2,s3,obj,tail");
    shouldBe(arr.length, 106);
}

// Default separator and the DFG/host path through Array.prototype.toString.
{
    const arr = makeArray();
    shouldBe(new Thread(a => String(a), arr).join(), "s0,s1,s2,s3,obj,tail");
}

// Already-segmented arrays take the generic join for every separator shape.
{
    const arr = ["a", "b", "c"];
    new Thread(a => { for (let i = 0; i < 64; ++i) a.push(i); }, arr).join();
    shouldBe(arr.length, 67);
    shouldBe(arr.join("").slice(0, 6), "abc012");
    shouldBe(arr.join(",").split(",").length, 67);
    shouldBe(arr.join("--").split("--")[66], "63");
    const doubles = [0.5, 1.5];
    new Thread(a => { for (let i = 0; i < 64; ++i) a.push(i + 0.25); }, doubles).join();
    shouldBe(doubles.join(",").split(",")[65], "63.25");
    const ints = [1, 2, 3];
    new Thread(a => { for (let i = 0; i < 64; ++i) a.push(i); }, ints).join();
    shouldBe(ints.join(",").split(",").length, 67);
}

// The other flat fast paths in ArrayPrototype keep their semantics when called
// from a foreign thread (owner-tagged flat words) and on segmented words.
{
    const arr = [1, 2, 3, 4, 5];
    shouldBe(new Thread(a => { a.reverse(); return a.join(","); }, arr).join(), "5,4,3,2,1");
    shouldBe(arr[0], 5);
    const doubles = [0.5, 1.5, 2.5];
    shouldBe(new Thread(a => { a.reverse(); return a[0]; }, doubles).join(), 2.5);
    const objects = [{ v: 1 }, { v: 2 }, { v: 3 }];
    new Thread(a => a.reverse(), objects).join();
    shouldBe(objects[0].v, 3);
    shouldBe(objects[2].v, 1);

    const grown = [1, 2, 3];
    new Thread(a => { for (let i = 4; i <= 70; ++i) a.push(i); }, grown).join();
    grown.reverse();
    shouldBe(grown[0], 70);
    shouldBe(grown[69], 1);
    shouldBe(grown.indexOf(70), 0);
    shouldBe(grown.lastIndexOf(1), 69);
    shouldBe(new Thread(a => a.indexOf(35), grown).join(), 35);
    shouldBe(new Thread(a => a.lastIndexOf(35), grown).join(), 35);
    shouldBe(grown.concat([0]).length, 71);
    shouldBe(grown.concat(0)[70], 0);
    shouldBe([9].concat(grown)[1], 70);
    shouldBe(new Thread(a => a.concat([8, 9]).length, grown).join(), 72);
}
