//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// spawned-thread-butterfly-stress.js — SPEC-jit Task 13: I14 spawned-thread
// butterfly stress (GIL-interleaved pre-integration).
//
// Each spawned thread runs JIT-warmed loops that:
//   1. allocate objects and transition them out-of-line (owner-thread
//      butterfly work — flag-on this exercises the §5.5 WRITE predicate's
//      owner case (2) with the thread's own R5 TID tag, NOT tag (0,0)),
//   2. publish them into a shared registry,
//   3. read OTHER threads' published objects (foreign READS — predicate's
//      mask-and-proceed case; no TID check on reads),
//   4. write to a designated shared object (foreign WRITES — case (4)
//      ensureSharedWriteBit then case (3) SW=1 stores).
// The main thread interleaves the same access mix plus GC pressure.
//
// Failure modes guarded: wrong values (mis-masked tag bits used as address
// bits), lost properties, crashes from a butterfly deref of a tagged word.

load("../harness.js", "caller relative");

const THREADS = 4;
const OBJECTS_PER_THREAD = 32;
const PROPS = 24; // out-of-line for any inline capacity
const ROUNDS = 60;

// registry[t] = array of objects published by thread t.
const registry = [];
for (let t = 0; t <= THREADS; ++t)
    registry.push([]);
const ready = { count: 0 };
const shared = { hits: 0 };

function buildOne(tid, serial) {
    const o = { tid: tid, serial: serial };
    for (let p = 0; p < PROPS; ++p)
        o["p" + p] = tid * 1000000 + serial * 1000 + p;
    const a = [];
    for (let p = 0; p < PROPS; ++p)
        a[p] = tid * 1000000 + serial * 1000 + p;
    o.indexed = a;
    return o;
}
noInline(buildOne);

function checkOne(o) {
    const base = o.tid * 1000000 + o.serial * 1000;
    let sum = 0;
    for (let p = 0; p < PROPS; ++p) {
        const named = o["p" + p];
        const idx = o.indexed[p];
        if (named !== base + p)
            throw new Error("named property corrupt: got " + named + " want " + (base + p));
        if (idx !== base + p)
            throw new Error("indexed property corrupt: got " + idx + " want " + (base + p));
        sum += named + idx;
    }
    return sum;
}
noInline(checkOne);

function threadBody(slot) {
    // Warm the loops on this thread (its own CodeBlock profiling under the
    // shared executable: §5.7 racy-profiling tolerance also gets exercise).
    for (let serial = 0; serial < OBJECTS_PER_THREAD; ++serial)
        registry[slot].push(buildOne(slot, serial));
    // Annex T2 (no preemptive-GIL reliance): Atomics rendezvous - a plain
    // `count++` is a two-step RMW that loses increments under true
    // parallelism and hangs this barrier (scaffolding only; oracle untouched).
    Atomics.add(ready, "count", 1);
    waitUntil(() => Atomics.load(ready, "count") > THREADS, 30000);

    let checksum = 0;
    for (let round = 0; round < ROUNDS; ++round) {
        // Own objects: re-verify + extend (owner transitions on a butterfly
        // other threads may concurrently be reading).
        const own = registry[slot];
        for (const o of own)
            checksum += checkOne(o);
        own[round % own.length]["late" + round] = slot * 7 + round;
        // Foreign reads: walk every other thread's published objects.
        for (let t = 0; t <= THREADS; ++t) {
            if (t === slot)
                continue;
            const theirs = registry[t];
            for (let i = 0; i < theirs.length; ++i)
                checksum += checkOne(theirs[i]);
        }
        // Foreign writes to the common shared object.
        Atomics.add(shared, "hits", 1);
        shared["fromSlot" + slot] = round;
        if (!(round % 16))
            sleepMs(1); // GIL drop: interleave with other threads mid-round
    }
    // Late-property integrity on own objects.
    for (let round = 0; round < ROUNDS; ++round) {
        const o = registry[slot][round % registry[slot].length];
        if (o["late" + round] !== slot * 7 + round)
            throw new Error("lost late property late" + round + " on slot " + slot);
    }
    return checksum;
}

// Main thread occupies slot THREADS.
const threads = spawnN(THREADS, threadBody);
const mainChecksum = threadBody(THREADS);
const results = joinAll(threads);

shouldBeTrue(mainChecksum > 0);
for (const r of results)
    shouldBeTrue(r > 0);
shouldBe(shared.hits, (THREADS + 1) * ROUNDS, "no lost Atomics.add on the shared object");
for (let t = 0; t <= THREADS; ++t)
    shouldBe(shared["fromSlot" + t], ROUNDS - 1, "last foreign write visible for slot " + t);

print("spawned-thread-butterfly-stress: PASS");
