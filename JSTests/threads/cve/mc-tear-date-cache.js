//@ requireOptions("--useJSThreads=1")
// MC-TEAR S7 (docs/threads/cve/map-MC-TEAR.md): DateInstance
// GregorianDateTime cache tear. UNGIL-HANDOUT §N.3 rules the cache BYPASSED
// GIL-off ("the cached pair is >8 bytes, not CASable") with m_data lazy
// alloc CAS-published. At audit time the bypass is NOT landed:
// DateInstance.cpp:44-73 plain-stores the RefPtr m_data (racing stores =
// refcount tear => over-release/UAF of DateInstanceData) and fills the >8B
// {m_gregorianDateTimeCachedForMS, m_cachedGregorianDateTime} pair with
// plain stores — a reader can pair the cachedForMS key from write A with
// date components from write B.
//
// Oracle: a shared Date flips between exactly TWO timestamps chosen so that
// EVERY individually-read component differs between them. Each component
// read must therefore belong to one of the two timestamps' component sets;
// any other value is a torn {key, components} cache pair. The m_data RefPtr
// race surfaces as a crash/ASAN UAF — the primary post-ungil signal.
// UTC accessors only, so the oracle is timezone-independent.
//
// WRITTEN DURING BRING-UP: do not execute until the GIL-off ladder is up.
// This test is the acceptance check for landing handout §N.3.
load("../harness.js", "caller relative");

const READERS = 3;
const FLIPS = 5000;

// Two timestamps differing in every UTC component we probe:
// A: 2001-03-05T04:06:07.008Z, B: 2014-10-21T17:38:49.501Z
const TS_A = Date.UTC(2001, 2, 5, 4, 6, 7, 8);
const TS_B = Date.UTC(2014, 9, 21, 17, 38, 49, 501);
const COMPONENTS = [
    ["getUTCFullYear", 2001, 2014],
    ["getUTCMonth", 2, 9],
    ["getUTCDate", 5, 21],
    ["getUTCHours", 4, 17],
    ["getUTCMinutes", 6, 38],
    ["getUTCSeconds", 7, 49],
    ["getUTCMilliseconds", 8, 501],
    ["getTime", TS_A, TS_B],
];

const shared = new Date(TS_A);
const box = { stop: 0, started: 0 };

const readers = spawnN(READERS, (id) => {
    Atomics.add(box, "started", 1);
    let reads = 0;
    while (Atomics.load(box, "stop") === 0) {
        for (const [name, a, b] of COMPONENTS) {
            const v = shared[name]();
            if (v !== a && v !== b)
                throw new Error("MC-TEAR S7: torn date-cache read: " + name
                    + "() = " + v + " (legal: " + a + " | " + b
                    + ", reader " + id + ")");
            reads++;
        }
        // toISOString round-trips the whole cached struct in one call; the
        // result must parse back to one of the two timestamps.
        const t = Date.parse(shared.toISOString());
        if (t !== TS_A && t !== TS_B)
            throw new Error("MC-TEAR S7: torn toISOString: " + t
                + " (reader " + id + ")");
    }
    return reads;
});

waitUntil(() => Atomics.load(box, "started") === READERS);

// Writer: flip between the two timestamps; each setTime invalidates the
// cached pair, each subsequent reader getter refills it — N concurrent
// fillers on the same DateInstance is the §N.3 race.
for (let i = 0; i < FLIPS; ++i) {
    shared.setTime((i & 1) ? TS_B : TS_A);
    if ((i & 255) === 0)
        sleepMs(0); // yield a slice under the cooperative phase-1 GIL
}

Atomics.store(box, "stop", 1);
const counts = joinAll(readers);
for (const c of counts)
    shouldBeTrue(c > 0, "every reader must have completed reads");
print("mc-tear-date-cache: PASS (" + counts.join(",") + " component reads)");
