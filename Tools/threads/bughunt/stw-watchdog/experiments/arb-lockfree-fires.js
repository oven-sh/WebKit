// arb-lockfree-fires.js — ARB-NEG-4 belt-and-braces synthetic (round 4).
//
// Goal: N threads generating ONLY lock-free Class-A fires at high frequency —
// Structure::didReplaceProperty via the putDirectInternal/defineOwnProperty
// path (no Baseline put-IC warmup on the replaced slot, so the retired
// Repatch didCachePropertyReplacement chain is never the fire site).
//
// Per batch, each thread:
//   1. builds a FRESH prototype P with a unique extra key (fresh Structure),
//   2. compiles a FRESH hot reader via Function() (fresh CodeBlock) that
//      warms a get_by_id on o.f found on P — caching the prototype access
//      installs ObjectPropertyConditions that watch P's replacement
//      WatchpointSet for slot f,
//   3. replaces P.f ONCE via Object.defineProperty (DefineOwnProperty ->
//      putDirectInternal -> didReplaceProperty fire of the now-watched set)
//      — a Class-A fire requested with NO ConcurrentJS lock held.
//
// Verify fire volume with JSC_CLASSA_FIRE_STATS=1 (env-gated counter in
// Watchpoint.cpp) and identities with JSC_CLASSA_FIRE_LOG=1
// ("Property did get replaced" stop fires).
//
// Expected on the landed-fix binary: 0 aborts, 0 hangs, 0 "requester queued"
// breadcrumbs; windows drain in ms. Any abort revives an arbitration-shaped
// defect independent of H1.

const N = (typeof globalThis.ARB_THREADS === "number") ? globalThis.ARB_THREADS : 4;
const BATCHES = (typeof globalThis.ARB_BATCHES === "number") ? globalThis.ARB_BATCHES : 120;

function workerBody(tid) {
    let acc = 0;
    for (let b = 0; b < BATCHES; ++b) {
        // Fresh prototype structure per batch (unique key => unique transition).
        const proto = {};
        proto["uniq_t" + tid + "_b" + b] = b;
        proto.f = 1;
        const o = Object.create(proto);
        o.own = b;
        // Fresh CodeBlock per batch so the get IC re-caches against the fresh
        // structure chain and re-installs the watchpoints every time.
        // Unique source per batch: defeats the code cache so each batch gets a
        // FRESH CodeBlock whose monomorphic get IC re-installs the proto-chain
        // watchpoints (a cached/shared CodeBlock goes megamorphic after ~16
        // structures and stops watching — measured: fires plateau at ~15).
        const reader = Function("o", "let s = " + (tid * 100000 + b) + " * 0; for (let i = 0; i < 3000; ++i) s += o.f; return s;");
        acc += reader(o);
        acc += reader(o); // second call: invocation-count-driven tier-up/IC adoption
        // Lock-free Class-A fire: replace the watched prototype slot via
        // DefineOwnProperty (NOT a hot put — no put-IC warmup on this slot).
        Object.defineProperty(proto, "f", { value: 2, writable: true, enumerable: true, configurable: true });
        acc += reader(o); // observe post-fire value; keeps the fire honest
    }
    return acc;
}

if (typeof Thread !== "function")
    throw new Error("Thread global missing (run with --useJSThreads=1)");

const threads = [];
for (let t = 0; t < N; ++t)
    threads.push(new Thread(workerBody, t));
let total = 0;
for (const th of threads)
    total += th.join();
print("ARB-LOCKFREE-FIRES done N=" + N + " batches=" + BATCHES + " total=" + total);
