//@ runDefault("--useConcurrentGC=false")
//@ runDefault("--useConcurrentGC=false", "--useJIT=false")

// What an embedder asks the heap when its program has gone quiet: how much has been allocated since the last full
// collection, against what was live then. Only a full collection starts the count over; an eden collection does not.

function assert(condition, what) {
    if (!condition)
        throw new Error(what);
}

fullGC();
const afterFull = $vm.allocationSinceLastFullCollection();
assert(afterFull.allocated < (1 << 20), "right after a full collection little has been allocated: " + afterFull.allocated);
assert(afterFull.liveThen > 0 && afterFull.budget > 0, "live size and budget are known: " + JSON.stringify(afterFull));

let keep = [];
for (let i = 0; i < 40; ++i)
    keep.push(new Array(32 * 1024).fill(i)); // ~10 MB of butterflies
const afterAllocating = $vm.allocationSinceLastFullCollection();
assert(afterAllocating.allocated >= 8 * (1 << 20), "the allocation shows: " + afterAllocating.allocated);

edenGC();
const afterEden = $vm.allocationSinceLastFullCollection();
// (The heap may turn the eden collection it was asked for into a full one; what was live "then" changes with that.)
if (afterEden.liveThen === afterAllocating.liveThen)
    assert(afterEden.allocated >= afterAllocating.allocated, "an eden collection does not start it over: " + afterEden.allocated + " after " + afterAllocating.allocated);
else
    assert(afterEden.allocated < (1 << 20), "it was a full collection after all: " + afterEden.allocated);

keep = null;
fullGC();
const afterSecondFull = $vm.allocationSinceLastFullCollection();
assert(afterSecondFull.allocated < (1 << 20), "a full collection does: " + afterSecondFull.allocated);
