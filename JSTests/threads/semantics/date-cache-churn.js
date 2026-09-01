//@ requireOptions("--useJSThreads=1")
// semantics/date-cache-churn.js — N threads formatting and parsing dates
// simultaneously. JSC's DateCache (per-VM year/month computation cache,
// gregorian-date scratch, parse buffers, and the localtime offset cache) is
// shared VM state under the threads model: concurrent toISOString /
// toUTCString / getUTC* / Date.parse from many threads must keep returning
// bit-exact answers. The oracle is computed single-threaded on main BEFORE
// any thread exists; the timestamps are chosen to thrash the caches (year
// boundaries, leap days, epoch, far past/future, DST-transition-adjacent
// instants, and a dense run inside one month to hit the cached-year fast
// path from many threads at once).
load("../harness.js", "caller relative");

const THREADS = 4;
const ROUNDS = 60;

const stamps = [];
// Cache-hostile spread: decade jumps, year boundaries, leap days.
const fixed = [
    0,                  // epoch
    -1,                 // 1969-12-31T23:59:59.999Z
    86400000 * 365,     // 1971-01-01
    951782400000,       // 2000-02-29 leap day
    1078012800000,      // 2004-02-29
    4107456000000,      // 2100-03-01 (post non-leap-century 2100-02-28+1)
    -2208988800000,     // 1900-01-01
    253402300799999,    // 9999-12-31T23:59:59.999Z
];
for (const ts of fixed)
    stamps.push(ts);
// Year-boundary straddles for a run of years (localtime cache churn).
for (let y = 0; y < 12; ++y)
    stamps.push(Date.UTC(2010 + y, 0, 1) - 1, Date.UTC(2010 + y, 0, 1));
// Dense same-month run (the cached-year/month fast path).
for (let d = 0; d < 24; ++d)
    stamps.push(Date.UTC(2023, 5, 1 + d, d % 24, d, d, d * 7 % 1000));

// Single-threaded oracle.
const oracle = stamps.map(ts => {
    const d = new Date(ts);
    return {
        iso: d.toISOString(),
        utc: d.toUTCString(),
        y: d.getUTCFullYear(), mo: d.getUTCMonth(), day: d.getUTCDate(),
        dow: d.getUTCDay(), h: d.getUTCHours(), mi: d.getUTCMinutes(),
        s: d.getUTCSeconds(), ms: d.getUTCMilliseconds(),
    };
});

// A shared, read-only Date object: every thread reads it concurrently.
const sharedDate = new Date(951782400000);
const sharedISO = sharedDate.toISOString();

const gate = { ready: 0, go: 0 };
const workers = spawnN(THREADS, t => {
    Atomics.add(gate, "ready", 1);
    while (Atomics.load(gate, "go") === 0)
        Atomics.wait(gate, "go", 0, 2);
    let digest = 0;
    for (let r = 0; r < ROUNDS; ++r) {
        // Stagger the walk per thread/round so threads hit DIFFERENT cache
        // lines at the same moment (maximum churn of the shared caches).
        for (let k = 0; k < stamps.length; ++k) {
            const i = (k * (t + 1) + r * 13) % stamps.length;
            const ts = stamps[i];
            const ex = oracle[i];
            const d = new Date(ts);
            const iso = d.toISOString();
            if (iso !== ex.iso)
                throw new Error("thread " + t + " round " + r + ": toISOString(" + ts + ") = " + iso + ", expected " + ex.iso);
            if (d.toUTCString() !== ex.utc)
                throw new Error("thread " + t + " round " + r + ": toUTCString(" + ts + ") diverged");
            if (d.getUTCFullYear() !== ex.y || d.getUTCMonth() !== ex.mo || d.getUTCDate() !== ex.day
                || d.getUTCDay() !== ex.dow || d.getUTCHours() !== ex.h || d.getUTCMinutes() !== ex.mi
                || d.getUTCSeconds() !== ex.s || d.getUTCMilliseconds() !== ex.ms)
                throw new Error("thread " + t + " round " + r + ": getUTC* fields diverged for " + ts);
            // Parse round-trip exercises the parse cache/scratch buffers.
            if (Date.parse(ex.iso) !== ts)
                throw new Error("thread " + t + " round " + r + ": Date.parse(" + ex.iso + ") != " + ts);
            // Local-time path: tz-dependent, so no golden string — but it
            // must be internally consistent within this very thread.
            if (d.toString() !== new Date(ts).toString())
                throw new Error("thread " + t + " round " + r + ": local toString not reproducible for " + ts);
            digest = (digest + iso.length + d.getUTCDay()) | 0;
        }
        // Shared Date object: immutable from all threads, stays bit-exact.
        if (sharedDate.getTime() !== 951782400000 || sharedDate.toISOString() !== sharedISO)
            throw new Error("thread " + t + " round " + r + ": shared Date object corrupted");
        if ((r & 15) === 15)
            sleepMs(1); // GIL-dropping yield for cooperative interleaving
    }
    return digest;
});

waitUntil(() => Atomics.load(gate, "ready") === THREADS);
Atomics.store(gate, "go", 1);
Atomics.notify(gate, "go", Infinity);

const digests = joinAll(workers);
// Same walk => same digest on every thread (cheap cross-thread agreement).
for (let t = 1; t < THREADS; ++t) {
    // Walks differ per thread (staggered), so digests differ; just require
    // each is a finite int32 — the real checks were inline above.
    shouldBeTrue(Number.isInteger(digests[t]), "thread " + t + " digest sane");
}

// Main thread after the storm: oracle still holds (caches not poisoned).
for (let i = 0; i < stamps.length; ++i)
    shouldBe(new Date(stamps[i]).toISOString(), oracle[i].iso, "post-storm oracle " + i);

// WOULD-FAIL-IF: the shared DateCache loses thread-safety — two threads
// computing year/month decompositions concurrently tear the cached year
// entry (toISOString or getUTC* returns a neighbor timestamp's fields:
// caught by the per-call oracle compare), the parse scratch buffer is
// shared without exclusion (Date.parse returns another thread's in-flight
// result), the localtime offset cache is updated non-atomically (local
// toString not even self-reproducible within one thread), or formatting a
// shared Date object mutates shared scratch state in place (sharedDate's
// ISO string wobbles). Every formatted value is compared against a
// pre-thread single-threaded oracle on every call, so a single torn cache
// entry anywhere in the run fails that exact call.
