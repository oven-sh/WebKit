//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// int-gate-fire-vs-execute.js — SPEC-jit Task 13 INTEGRATION GATE:
// true-concurrent Class-A watchpoint-fire-vs-execute stress (§5.6).
//
// Scaled-down smoke by default while STWR is stubbed; run with `-- int-gate`
// for the full loops at M4/CS2 (N-separate-VMs config only, R1 freeze scope).
//
// Shape: worker threads execute code DEPENDENT on watchable conditions
// (prototype-property loads with adaptive watchpoints; array-shape
// speculation) while the conductor thread fires the underlying Class-A sets:
//   - replacement-set fires (prototype property overwrites),
//   - transition-set fires (structure chain invalidations via dictionary
//     round-trips),
//   - havingABadTime (global Class-A fire) against a scratch global.
// Acceptance (post-M4): the fireAllUnderClassAStop RELEASE_ASSERTs
// (serviced + invalidated) hold, the App. 5.6(d) watchdog stays quiet, every
// worker observes the post-fire value after the fire completes (synchronous
// completion is load-bearing, history §13.5), and fires coalesce (stop
// count <= fire count; measured by int-gate-stop-budget.js).

load("../harness.js", "caller relative");

const FULL = typeof arguments !== "undefined" && Array.prototype.indexOf.call(arguments, "int-gate") >= 0;
const ROUNDS = FULL ? 300 : 10;
const THREADS = FULL ? 4 : 2;

// Generation-stamped prototype: workers read o.f through the prototype; the
// conductor bumps proto.f (FIRES the replacement set each time after an IC
// watched it). Workers must observe a MONOTONIC sequence of generations —
// observing generation g-1 after some thread observed g would mean a fired
// IC kept serving the stale constant past fire completion.
const proto = { f: 0 };
const instances = [];
for (let i = 0; i < 8; ++i) {
    const o = Object.create(proto);
    o["own" + i] = i; // distinct structures => several watched chains
    instances.push(o);
}

const published = { generation: 0 };
const stop = { value: false };

function readThroughProto(o) { return o.f; }
noInline(readThroughProto);

// Warm: install the prototype-load ICs + adaptive watchpoints.
for (let i = 0; i < 20000; ++i) {
    if (readThroughProto(instances[i & 7]) !== 0)
        throw new Error("warmup");
}

const workers = spawnN(THREADS, function (slot) {
    let maxSeen = 0;
    let reads = 0;
    while (!stop.value) {
        const floor = published.generation; // published BEFORE the fire-write below
        const v = readThroughProto(instances[reads & 7]);
        if (v < floor)
            throw new Error("worker " + slot + " observed PRE-FIRE value " + v + " after generation " + floor + " was published (stale fired code ran)");
        if (v < maxSeen)
            throw new Error("worker " + slot + " observed non-monotonic generations " + v + " < " + maxSeen);
        maxSeen = v;
        ++reads;
        if (!(reads % 64))
            sleepMs(0);
    }
    return reads;
});

for (let round = 1; round <= ROUNDS; ++round) {
    // Publish the floor FIRST: any read that starts after this line may
    // legally still see round-1 until the fire completes, but never less
    // than the floor we publish (floor = previous generation).
    published.generation = round - 1;
    proto.f = round; // Class-A fire: replacement set on proto's structure
    published.generation = round; // fire completed synchronously on this thread
    // Periodic transition-set fires on the instance structures.
    if (typeof $vm !== "undefined" && $vm.toCacheableDictionary && (round % 40) === 0) {
        const victim = Object.create(proto);
        victim.x = 1;
        readThroughProto(victim);
        $vm.toCacheableDictionary(victim);
        if ($vm.flattenDictionaryObject)
            $vm.flattenDictionaryObject(victim);
    }
    if (typeof $vm !== "undefined" && $vm.createGlobalObject && $vm.haveABadTime && (round % 100) === 0)
        $vm.haveABadTime($vm.createGlobalObject());
    if (!(round % 8))
        sleepMs(1);
}

stop.value = true;
const reads = joinAll(workers);
for (const r of reads)
    shouldBeTrue(r > 0, "every worker made progress across fires");
shouldBe(readThroughProto(instances[0]), ROUNDS);
print("int-gate-fire-vs-execute: PASS (" + (FULL ? "FULL" : "smoke — rerun with -- int-gate at M4/CS2") + ")");
