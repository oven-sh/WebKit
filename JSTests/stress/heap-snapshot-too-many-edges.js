//@ memoryHog!
//@ skip if $memoryLimited
//@ runDefault

// A heap snapshot keeps its edges in one Vector, whose buffer cannot be larger than 2^31 bytes: 53,687,091 edges
// at most. A snapshot of a heap with more edges has to fail. It must not abort the process.

// 54 arrays of 1,000,000 elements that reference one object: 54,000,000 Index edges.
let shared = { };
let arrays = [];
for (let i = 0; i < 54; ++i)
    arrays.push(new Array(1000000).fill(shared));

let error;
try {
    generateHeapSnapshot();
} catch (e) {
    error = e;
}
if (!(error instanceof RangeError) || error.message !== "Out of memory")
    throw new Error("generateHeapSnapshot() should throw an out of memory error, not " + error);
