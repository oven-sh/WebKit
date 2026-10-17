//@ runDefault
//@ runDefault("--useConcurrentGC=0")
//@ runDefault("--useJIT=0")
//@ runDefault("--jitPolicyScale=0", "--useConcurrentJIT=0")
//@ runDefault("--collectContinuously=1", "--useConcurrentGC=1")
//@ runDefault("--collectContinuously=1", "--useConcurrentGC=0")
//@ runDefault("--forceGCSlowPaths=1")
//@ runDefault("--slowPathAllocsBetweenGCs=50")
//@ runDefault("--evacuateAuxiliaryBlocksAfterEveryFullCollection=1")

// Heap::startAllocationSampling, through the shell's test client (ShellAllocationSampler in jsc.cpp).

function assert(condition, message) {
    if (!condition)
        throw new Error(message);
}

function assertBetween(value, low, high, message) {
    assert(value >= low && value <= high, message + ": " + value + " is not within [" + low + ", " + high + "]");
}

let keep;

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

// Warm the allocators and compile the loops before sampling starts, so the inline allocators are the ones under test.
for (let i = 0; i < 100; ++i) {
    allocateObjects(200);
    allocateArrays(200);
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
    const live = heapAllocationSamples();
    assert(live.samples > 0, "no samples at interval " + interval);
    assert(live.liveCells + live.deadCells === live.samples - live.extraMemory, "a cell sample is either live or dead");
    assert(live.liveCells > live.samples / 2, "most sampled cells are live while the loops' arrays are kept");
    keep = null;
    // The frames of the two loops are dead, but a collection that the collector thread conducts scans the mutator's
    // stack as it is, and a slot of a dead frame can still hold the last array. Run the loops again, small, so the
    // slots hold small arrays instead.
    allocateObjects(10);
    allocateArrays(10);
    keep = null;
    fullGC();
    const collected = heapAllocationSamples();
    // The sampled cells of the loops are dead now; the few live ones belong to the harness.
    assert(collected.liveCells < live.liveCells / 20, "dead cells stay live: " + live.liveCells + " -> " + collected.liveCells);
    assert(collected.deadCells >= live.liveCells - collected.liveCells, "dead cells are not counted");
    const result = stopHeapAllocationSampling();
    assert(result.samples >= collected.samples, "stop returns the last state");
    assert(heapAllocationSamples() === null, "stop turns sampling off");
    estimates.push(result.estimatedBytes);
    counts.push(result.samples);
}
assertBetween(counts[0] / counts[1], 5, 11, "samples at 128 vs 1024 bytes");
assertBetween(counts[1] / counts[2], 20, 45, "samples at 1024 vs 32768 bytes");
for (const estimate of estimates)
    assertBetween(estimate / estimates[0], 0.8, 1.25, "estimate at another interval vs the estimate at 128 bytes");

// Sampling that starts while the allocators hold free lists, and stops while they do.
startHeapAllocationSampling(256);
allocateObjects(1000);
assert(heapAllocationSamples().samples > 0, "samples after a start in the middle of allocation");
stopHeapAllocationSampling();
allocateObjects(1000);
startHeapAllocationSampling(256);
allocateObjects(1000);
assert(heapAllocationSamples().samples > 0, "samples after a restart");
stopHeapAllocationSampling();

// An eden collection settles the dead cells of the eden too. Under an option that collects while the loop runs, the
// cells are old by the time they die, and the full collection after it finds them.
startHeapAllocationSampling(512);
allocateObjects(20000);
const beforeEden = heapAllocationSamples();
keep = null;
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
keep = [];
for (let i = 0; i < 300000; ++i)
    keep.push(i);
result = heapAllocationSamples();
assert(result.relocated > 0, "growth of a precise allocation did not move it");
fullGC();
const afterGrowth = heapAllocationSamples();
assert(afterGrowth.liveCells > 0, "the grown storage is live but its samples are not");
keep = null;
fullGC();
assert(heapAllocationSamples().liveCells < afterGrowth.liveCells, "the dead storage stays live");
stopHeapAllocationSampling();

// Strings: the characters are extra memory, small payloads included.
const shortStrings = Array.from({ length: 20000 }, (_, i) => '"' + "\\u0061".repeat(36) + "\\u" + i.toString(16).padStart(4, "0") + '"');
function parseShortStrings() {
    keep = new Array(shortStrings.length);
    for (let i = 0; i < shortStrings.length; ++i)
        keep[i] = JSON.parse(shortStrings[i]);
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
    allocateArrays(5000);
    if (i % 2)
        fullGC();
    else
        edenGC();
}
keep = null;
fullGC();
assert(heapAllocationSamples().deadCells > 0, "no dead cells after many collections");
stopHeapAllocationSampling();
