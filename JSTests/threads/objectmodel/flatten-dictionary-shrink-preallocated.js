//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// Flag-on, flattening an uncacheable dictionary runs under a stop-the-world
// window whose closure must not allocate in the GC heap. When the compaction
// drops enough out-of-line capacity to need a smaller flat butterfly while
// keeping out-of-line storage or an indexing header, the shift leg
// (JSObject::shiftButterflyAfterFlattening) publishes a butterfly that
// Structure::flattenDictionaryStructureUnderStop allocates before the stop
// from a sizing plan it re-derives inside. This exercises every shape that
// plan covers and checks named and indexed storage survive the shift.
load("../harness.js", "caller relative");

const PAD = 40;

function addNamed(o, count, prefix) {
    for (let i = 0; i < count; ++i)
        o[prefix + i] = prefix + ":" + i;
}

// Dictionary-mode deletes of the upper half: post-compaction out-of-line size
// is about half the pre-flatten capacity, so the capacity bucket shrinks.
function makeShrinkingDictionary(o, count, prefix) {
    $vm.toUncacheableDictionary(o);
    for (let i = count / 2; i < count; ++i)
        delete o[prefix + i];
}

function checkNamed(o, count, prefix) {
    for (let i = 0; i < count / 2; ++i)
        shouldBe(o[prefix + i], prefix + ":" + i);
    for (let i = count / 2; i < count; ++i)
        shouldBe(o[prefix + i], undefined);
}

// Plain object: out-of-line storage shrinks but stays non-empty.
{
    const o = {};
    addNamed(o, PAD, "p");
    makeShrinkingDictionary(o, PAD, "p");
    $vm.flattenDictionaryObject(o);
    checkNamed(o, PAD, "p");
    o.after = 1;
    shouldBe(o.after, 1);
}

// Contiguous array: indexing header with a vector payload, no pre-capacity.
{
    const a = [];
    for (let i = 0; i < 16; ++i)
        a[i] = i * 3;
    shouldBe($vm.indexingMode(a), "ArrayWithInt32");
    addNamed(a, PAD, "q");
    makeShrinkingDictionary(a, PAD, "q");
    $vm.flattenDictionaryObject(a);
    checkNamed(a, PAD, "q");
    shouldBe(a.length, 16);
    for (let i = 0; i < 16; ++i)
        shouldBe(a[i], i * 3);
}

// Array storage with pre-capacity: a sparse put forces ArrayStorage and the
// unshift leaves an index bias in front of the vector.
{
    const a = [];
    for (let i = 0; i < 16; ++i)
        a[i] = "e" + i;
    a[100000] = "sparse";
    a.unshift("front");
    shouldBe($vm.indexingMode(a), "ArrayWithArrayStorage");
    addNamed(a, PAD, "r");
    makeShrinkingDictionary(a, PAD, "r");
    $vm.flattenDictionaryObject(a);
    checkNamed(a, PAD, "r");
    shouldBe(a.length, 100002);
    shouldBe(a[0], "front");
    for (let i = 0; i < 16; ++i)
        shouldBe(a[i + 1], "e" + i);
    shouldBe(a[100001], "sparse");
}

// Every out-of-line property deleted with an indexing header left: the
// butterfly is shifted down to the header alone rather than dropped.
{
    const a = [1, 2, 3];
    addNamed(a, PAD, "s");
    $vm.toUncacheableDictionary(a);
    for (let i = 0; i < PAD; ++i)
        delete a["s" + i];
    $vm.flattenDictionaryObject(a);
    shouldBe(a.s0, undefined);
    shouldBe(a.length, 3);
    shouldBe(a[2], 3);
    a.after = 2;
    shouldBe(a.after, 2);
}

// Flattened from a foreign thread: the pre-allocation happens on the
// flattening thread and the object keeps its owner tag.
{
    const o = {};
    addNamed(o, PAD, "t");
    makeShrinkingDictionary(o, PAD, "t");
    new Thread(() => { $vm.flattenDictionaryObject(o); }).join();
    checkNamed(o, PAD, "t");
    o.after = 3;
    shouldBe(o.after, 3);
}
