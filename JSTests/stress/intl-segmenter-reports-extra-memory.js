//@ memoryHog!
//@ runDefault

// Intl.Segmenter, the Segments object segment() returns and the iterator Segments creates each own ICU memory, and
// Segments additionally a UTF-16 copy of the whole input. All of it has to be reported to the GC: gcHeapSize() is the
// live size the GC saw in its last collection, including the extra memory cells reported, and the same numbers drive
// when the GC decides to collect.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: expected ${expected}, got ${actual}`);
}

function liveHeapSize() {
    fullGC();
    return gcHeapSize();
}

const input = "a".repeat(1 << 20);
const bufferBytes = input.length * 2; // the copy is UTF-16 even for a Latin-1 input
const segmenter = new Intl.Segmenter("en");

{
    const base = liveHeapSize();
    const segments = segmenter.segment(input);
    const withSegments = liveHeapSize() - base;
    shouldBe(withSegments >= bufferBytes, true, `Segments reports its buffer (grew by ${withSegments})`);
    shouldBe(withSegments < 2 * bufferBytes, true, `Segments reports its buffer once (grew by ${withSegments})`);

    const iterator = segments[Symbol.iterator]();
    const withIterator = liveHeapSize() - base;
    shouldBe(withIterator >= bufferBytes, true, `buffer is still reported with an iterator alive (grew by ${withIterator})`);
    shouldBe(withIterator < 2 * bufferBytes, true, `iterator does not report the shared buffer again (grew by ${withIterator})`);

    shouldBe(iterator.next().value.segment, "a", "iterator still works");
    shouldBe(segments.containing(5).index, 5, "segments still works");
}

// Each break iterator ICU allocates is around 1.5 KB (a clone) to 7 KB (a fresh one); the cells holding them are
// a few dozen bytes. 1000 of them have to show up as at least 1 MB of live heap.
{
    const count = 1000;
    const base = liveHeapSize();
    const segmenters = [];
    for (let i = 0; i < count; ++i)
        segmenters.push(new Intl.Segmenter("en", { granularity: "word" }));
    const withSegmenters = liveHeapSize() - base;
    shouldBe(withSegmenters >= count * 1000, true, `${count} Segmenters report their break iterators (grew by ${withSegmenters})`);

    const segments = segmenter.segment("");
    const iterators = [];
    for (let i = 0; i < count; ++i)
        iterators.push(segments[Symbol.iterator]());
    const withIterators = liveHeapSize() - withSegmenters - base;
    shouldBe(withIterators >= count * 1000, true, `${count} iterators report their break iterators (grew by ${withIterators})`);

    shouldBe(segmenters.length + iterators.length, 2 * count, "everything is still alive");
}

// The reporting is what makes the GC run at all in a loop like this: every call copies the 1 MB input into a 2 MB
// buffer and allocates nothing else the GC can see but a small cell, so without it the loop holds on to all 512 MB
// until something unrelated happens to trigger a collection.
{
    const footprintBefore = MemoryFootprint().current;
    const count = 256;
    for (let i = 0; i < count; ++i)
        segmenter.segment(input);
    const growth = MemoryFootprint().current - footprintBefore;
    shouldBe(growth < (count * bufferBytes) / 4, true, `dropped Segments are collected while the loop runs (footprint grew by ${growth} of ${count * bufferBytes} allocated)`);
}
