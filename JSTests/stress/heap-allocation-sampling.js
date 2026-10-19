//@ runDefault
//@ runDefault("--useConcurrentGC=0")
//@ runDefault("--useJIT=0")
//@ runDefault("--jitPolicyScale=0", "--useConcurrentJIT=0")
//@ runDefault("--collectContinuously=1", "--useConcurrentGC=1")
//@ runDefault("--collectContinuously=1", "--useConcurrentGC=0")
//@ runDefault("--forceGCSlowPaths=1")
//@ runDefault("--slowPathAllocsBetweenGCs=50")
//@ runDefault("--evacuateAuxiliaryBlocksAfterEveryFullCollection=1")
//@ runDefault("--verifyGC=1")

// Heap::startAllocationSampling, through the shell's test client (ShellAllocationSampler in jsc.cpp).
//
// What must die here is garbage that nothing can reach once its loop has returned: a loop that drops every object
// but the last. A loop that fills an array and hands it to a global is only for what must stay alive. A dropped
// array can still be in a slot of a dead frame, and a collection conducted by the collector thread scans the stack
// as it is, so a test that drops an array and expects its contents dead is not deterministic.

function assert(condition, message) {
    if (!condition)
        throw new Error(message);
}

function assertBetween(value, low, high, message) {
    assert(value >= low && value <= high, message + ": " + value + " is not within [" + low + ", " + high + "]");
}

let keep;
let sink;

function allocateObjects(count) {
    const values = [];
    for (let i = 0; i < count; ++i)
        values.push({ index: i, a: i + 1, b: i + 2 });
    keep = values;
}
noInline(allocateObjects);

function allocateArrays(count) {
    const values = [];
    for (let i = 0; i < count; ++i)
        values.push([i, i + 1, i + 2, i + 3]);
    keep = values;
}
noInline(allocateArrays);

function dropObjects(count) {
    for (let i = 0; i < count; ++i)
        sink = { index: i, a: i + 1, b: i + 2 };
}
noInline(dropObjects);

function dropArrays(count) {
    for (let i = 0; i < count; ++i)
        sink = [i, i + 1, i + 2, i + 3];
}
noInline(dropArrays);

// Warm the allocators and compile the loops before sampling starts, so the inline allocators are the ones under test.
for (let i = 0; i < 100; ++i) {
    allocateObjects(200);
    allocateArrays(200);
    dropObjects(200);
    dropArrays(200);
}
keep = null;
fullGC();

assert(stopHeapAllocationSampling() === null, "stop without start");
assert(heapAllocationSamples() === null, "samples without start");

// The estimate of the bytes allocated does not depend on the interval, and the number of samples does.
const estimates = [];
const counts = [];
for (const interval of [128, 1024, 32768]) {
    startHeapAllocationSampling(interval);
    allocateObjects(100000);
    allocateArrays(100000);
    const kept = heapAllocationSamples();
    assert(kept.samples > 0, "no samples at interval " + interval);
    assert(kept.liveCells + kept.deadCells === kept.samples - kept.extraMemory, "a cell sample is either live or dead");
    fullGC();
    const stillKept = heapAllocationSamples();
    // The arrays are in `keep`: their cells are still live, whatever the old array storage that grew them did.
    assert(stillKept.liveCells > kept.samples / 3, "kept cells died: " + kept.liveCells + " -> " + stillKept.liveCells + " of " + kept.samples);
    dropObjects(100000);
    dropArrays(100000);
    const dropped = heapAllocationSamples();
    fullGC();
    const collected = heapAllocationSamples();
    // Every dropped object but the last is garbage. Some may have died in a collection during the loops already.
    const droppedSamples = dropped.samples - kept.samples;
    assert(droppedSamples > 0, "no samples for the dropped objects");
    assert(collected.deadCells - stillKept.deadCells > droppedSamples * 0.8, "dead cells stay live: " + (collected.deadCells - stillKept.deadCells) + " of " + droppedSamples + " died");
    assert(collected.liveCells > kept.samples / 3, "kept cells died with the dropped ones");
    const result = stopHeapAllocationSampling();
    assert(result.samples >= collected.samples, "stop returns the last state");
    assert(heapAllocationSamples() === null, "stop turns sampling off");
    estimates.push(result.estimatedBytes);
    counts.push(result.samples);
    keep = null;
}
assertBetween(counts[0] / counts[1], 5, 11, "samples at 128 vs 1024 bytes");
assertBetween(counts[1] / counts[2], 20, 45, "samples at 1024 vs 32768 bytes");
for (const estimate of estimates)
    assertBetween(estimate / estimates[0], 0.8, 1.25, "estimate at another interval vs the estimate at 128 bytes");

// Sampling that starts while the allocators hold free lists, and stops while they do.
startHeapAllocationSampling(256);
dropObjects(1000);
assert(heapAllocationSamples().samples > 0, "samples after a start in the middle of allocation");
stopHeapAllocationSampling();
dropObjects(1000);
startHeapAllocationSampling(256);
dropObjects(1000);
assert(heapAllocationSamples().samples > 0, "samples after a restart");
stopHeapAllocationSampling();

// An eden collection settles the dead cells of the eden too. Under an option that collects while the loop runs, the
// cells are old by the time the eden collection comes, and the full collection after it finds them.
startHeapAllocationSampling(512);
dropObjects(20000);
const beforeEden = heapAllocationSamples();
edenGC();
const afterEden = heapAllocationSamples();
assert(afterEden.liveCells + afterEden.deadCells === afterEden.samples - afterEden.extraMemory, "a cell sample is either live or dead after an eden collection");
fullGC();
assert(heapAllocationSamples().deadCells > beforeEden.deadCells, "no collection finds the dead sampled cells");
stopHeapAllocationSampling();

// A large array's storage is a precise allocation, and growing it is growth of one.
startHeapAllocationSampling(4096);
keep = new Array(1000000).fill(1);
let result = heapAllocationSamples();
assert(result.cellBytes >= 8000000, "the precise allocation of a large array is not sampled: " + result.cellBytes);
const beforeGrowth = heapAllocationSamples();
keep = [];
for (let i = 0; i < 300000; ++i)
    keep.push(i);
result = heapAllocationSamples();
// Each time the storage outgrows its allocation, the added bytes are a pick against the byte budget.
assert(result.cellBytes > beforeGrowth.cellBytes + 1000000, "growth of a precise allocation is not sampled: " + beforeGrowth.cellBytes + " -> " + result.cellBytes);
fullGC();
assert(heapAllocationSamples().liveCells > 0, "the grown storage is live but its samples are not");
// A large array that nothing keeps: the loop's last array is the one that may linger, so end with a small one.
function dropLargeArray(length) {
    sink = new Array(length).fill(1);
}
noInline(dropLargeArray);
const beforeDrop = heapAllocationSamples();
dropLargeArray(1000000);
dropLargeArray(10);
fullGC();
assert(heapAllocationSamples().deadCells > beforeDrop.deadCells, "the dropped large storage stays live");
stopHeapAllocationSampling();

// Strings: the characters are extra memory, small payloads included.
const shortStringInputs = Array.from({ length: 20000 }, (_, i) => '"' + "\\u0061".repeat(36) + "\\u" + i.toString(16).padStart(4, "0") + '"');
function parseShortStrings() {
    keep = new Array(shortStringInputs.length);
    for (let i = 0; i < shortStringInputs.length; ++i)
        keep[i] = JSON.parse(shortStringInputs[i]);
}
noInline(parseShortStrings);
function parseLongStrings() {
    keep = new Array(2048);
    for (let i = 0; i < keep.length; ++i)
        keep[i] = JSON.parse('"' + "\\u0061".repeat(4096) + '"');
}
noInline(parseLongStrings);
for (const [allocate, minimumBytes] of [[parseShortStrings, 500000], [parseLongStrings, 8000000]]) {
    startHeapAllocationSampling(256);
    allocate();
    const strings = heapAllocationSamples();
    assert(strings.extraMemory > 0, allocate.name + ": no extra memory samples");
    assert(strings.extraBytes >= minimumBytes, allocate.name + ": extra memory bytes " + strings.extraBytes + " < " + minimumBytes);
    keep = null;
    fullGC();
    stopHeapAllocationSampling();
}

// Sampling survives the heap being collected many times over.
startHeapAllocationSampling(1024);
for (let i = 0; i < 20; ++i) {
    dropArrays(5000);
    if (i % 2)
        fullGC();
    else
        edenGC();
}
fullGC();
assert(heapAllocationSamples().deadCells > 0, "no dead cells after many collections");
stopHeapAllocationSampling();
