//@ requireOptions("--useJSThreads=1")
// map-heavy: Map/Set churn — hashing, bucket allocation, rehash growth,
// tombstone compaction, and iteration.
//
// Each thread churns its OWN Map and Set through repeated generations:
// populate with mixed number/string keys, delete an interleaved half
// (tombstones), re-insert a quarter (forcing probe sequences over deleted
// entries), then iterate in insertion order folding keys and values into the
// checksum (iteration order is spec-deterministic, so the checksum is exact).
// Each generation drops the previous Map/Set, so the hash-table backing
// stores themselves are the dominant allocation. NO data is shared between
// threads.
//
// Gate class: standard thresholds (2.8x@4 / 4.5x@8). The string keys are
// rope-free single-segment strings; their atomization traffic is incidental
// and far lighter than string-heavy's.
//
// Work size targets roughly 1s per thread on a release build at scale 1 (the gate's sizing; standalone corpus runs default to a fractional CORPUS_DEFAULT_SCALE - see harness.js);
// fixed generation count, no blocking ops.
load("./harness.js", "caller relative");

function mapWorkload() {
    const GENERATIONS = Math.round(700 * scalingWorkScale());
    const ENTRIES = 4000;

    let checksum = 0;
    function fold(value) {
        checksum = (checksum * 33 + value) % 0x7fffffff;
    }

    for (let gen = 0; gen < GENERATIONS; ++gen) {
        const map = new Map();
        const set = new Set();

        // 1) Populate: alternating number and string keys; growth rehashes.
        for (let i = 0; i < ENTRIES; ++i) {
            const n = (i * 2654435761 + gen) >>> 0;
            if ((i & 1) === 0) {
                map.set(n, i);
                set.add(n & 0xffff);
            } else {
                map.set("k" + (n & 0xfffff), i);
                set.add("s" + (n & 0x3fff));
            }
        }

        // 2) Delete an interleaved half: tombstones spread over every bucket
        //    region rather than one contiguous run.
        let deleted = 0;
        for (let i = 0; i < ENTRIES; i += 2) {
            const n = (i * 2654435761 + gen) >>> 0;
            if (map.delete(n))
                deleted++;
        }

        // 3) Re-insert a quarter: probes walk the deleted slots.
        for (let i = 0; i < ENTRIES; i += 4) {
            const n = (i * 2654435761 + gen) >>> 0;
            map.set(n, -i);
        }

        // 4) Lookups: hit and miss paths.
        let hits = 0;
        for (let i = 0; i < ENTRIES; ++i) {
            const n = (i * 2654435761 + gen) >>> 0;
            if (map.has(n) || map.has("k" + (n & 0xfffff)))
                hits++;
        }

        // 5) Iterate in insertion order; fold sizes, keys, and values.
        let numericSum = 0;
        let stringKeys = 0;
        for (const [key, value] of map) {
            if (typeof key === "number")
                numericSum = (numericSum + (key & 0xffff) + value) | 0;
            else
                stringKeys++;
        }
        let setProbe = 0;
        for (const member of set)
            setProbe = (setProbe + (typeof member === "number" ? member : member.length)) | 0;

        fold(map.size);
        fold(set.size);
        fold(deleted);
        fold(hits);
        fold(numericSum >>> 0);
        fold(stringKeys);
        fold(setProbe >>> 0);
    }

    return "m" + checksum;
}

runScalingWorkload("map-heavy", mapWorkload);

// WOULD-FAIL-IF: the Map/Set machinery picks up a cross-thread serial
// section or a concurrency bug — e.g. HashMapImpl/OrderedHashTable bucket
// stores funneling through a shared lock or a serialized barrier/slow path,
// string-key hashing contending on shared hash/atom state, or rehash-in-GC
// interactions (hash tables moving under concurrent collection) stalling all
// mutators. Throughput collapse trips scaling-gate.sh --gate's 2.8@4 / 4.5@8
// floor on exactly this workload while richards-like still scales (isolating
// it to the collection types) — NOTE this half of the claim is live ONLY
// when the pinned --gate rung runs (see Tools/threads/INTEGRATE-scaling.md;
// default corpus runs check only the checksum half, plus the opt-in
// SCALING_SELF_TRIPWIRE in harness.js for gross re-serialization).
// Standalone, a rehash or tombstone-compaction
// race that drops, duplicates, or reorders entries in a thread-LOCAL map
// changes that thread's size/iteration-order checksum and fails the
// harness's comparison against the single-thread reference.
