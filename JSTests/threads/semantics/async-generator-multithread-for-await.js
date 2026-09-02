//@ requireOptions("--useJSThreads=1")
// semantics/async-generator-multithread-for-await.js — a for await loop on
// every thread over ONE async generator. The loops use the engine's fused
// driver path (enqueueAsyncGeneratorDriver), not next(): the loop's own
// generator or async function is queued as the request's settlement target.
// The invariants are those of async-generator-multithread-requests.js: every
// value reaches exactly one loop, every loop ends, and the body never runs on
// two threads at once.
//
// GIL-off this file also runs two different generator bodies at once (the
// shared one, and each odd thread's relay). Both are entered through the
// per-VM MicrotaskCallCache (VM::syncResumeCallCache), which is not safe to
// use from two threads; see docs/threads/REVIEW-async-generator.md.
load("../harness.js", "caller relative");

const THREADS = 4;
const DEADLINE_MS = 60000;

function makeShared(total) {
    return {
        lock: new Lock(),
        inBody: 0,
        overlap: 0,
        yields: 0,
        settled: 0,
        doneFalse: 0,
        badCount: 0,
        firstBad: null,
        resolver: null,
        settleCounts: new Int32Array(new SharedArrayBuffer(4 * total)),
    };
}

function bad(s, message) {
    if (Atomics.add(s, "badCount", 1) === 0)
        s.lock.hold(() => { s.firstBad = message; });
}

// Called on entry to a stretch of body code, and after each resumption.
function enter(s) {
    if (Atomics.add(s, "inBody", 1) !== 0)
        Atomics.add(s, "overlap", 1);
}

// Called before each suspension, and when the body finishes.
function leave(s) {
    Atomics.sub(s, "inBody", 1);
}

// A promise that a thread other than the body's own may resolve (see poke), so
// the await that uses it can resume on another thread. Only one body runs at a
// time, so one slot is enough. If two ran at once, one resolver would be lost
// and the run would hit the deadline.
function crossThreadTick(s) {
    return new Promise(resolve => {
        s.lock.hold(() => { s.resolver = resolve; });
    });
}

function poke(s) {
    const resolve = s.lock.hold(() => {
        const r = s.resolver;
        s.resolver = null;
        return r;
    });
    if (resolve)
        resolve();
}

function drainUntilSettled(s, total) {
    const deadline = Date.now() + DEADLINE_MS;
    let last = -1;
    for (;;) {
        drainMicrotasks();
        poke(s);
        const n = Atomics.load(s, "settled");
        if (n >= total)
            break;
        if (Date.now() > deadline)
            throw new Error("only " + n + " of " + total + " requests settled: a request was queued and never run");
        // GIL-on, threads switch only at blocking primitives, so block when
        // nothing moved.
        if (n === last)
            sleepMs(1);
        last = n;
    }
    drainMicrotasks();
}

function checkCommon(s, total, label) {
    shouldBe(s.badCount, 0, label + ": " + s.firstBad);
    shouldBe(s.overlap, 0, label + ": the body ran on two threads at once");
    shouldBe(s.inBody, 0, label + ": body entry and exit counts differ");
    shouldBe(s.settled, total, label + ": settled count");
    for (let i = 0; i < total; ++i)
        shouldBe(s.settleCounts[i], 1, label + ": settlements of request " + i);
}

// Each value in [0, k) was delivered once, and nothing at or above k was.
function checkDelivered(s, seen, k, label) {
    for (let v = 0; v < seen.length; ++v)
        shouldBe(seen[v], v < k ? 1 : 0, label + ": deliveries of value " + v);
    shouldBe(s.doneFalse, k, label + ": results with done false");
}

// Yields 0, 1, ... n-1. Awaits between some yields: a resolved value, and a
// promise that another thread may resolve.
async function* countingBody(s, n) {
    enter(s);
    try {
        for (let i = 0; i < n; ++i) {
            if (i % 4 === 1) {
                leave(s);
                try { await null; } finally { enter(s); }
            } else if (i % 4 === 3) {
                leave(s);
                try { await crossThreadTick(s); } finally { enter(s); }
            }
            Atomics.add(s, "yields", 1);
            leave(s);
            try { yield i; } finally { enter(s); }
        }
    } finally {
        leave(s);
    }
}

// Runs worker(t) on THREADS threads and drains on main until all settle.
function runOnThreads(s, total, worker) {
    const threads = spawnN(THREADS, t => {
        worker(t);
        drainUntilSettled(s, total);
        return t;
    });
    drainUntilSettled(s, total);
    joinAll(threads);
    drainMicrotasks();
}

// Odd threads go through an async generator of their own, so that both kinds
// of driver are used.
{
    const N = 200;
    const s = makeShared(THREADS);
    const seen = new Int32Array(new SharedArrayBuffer(4 * N));
    const g = countingBody(s, N);

    async function* relay(source) {
        for await (const v of source)
            yield v;
    }

    // Make one relay on main first. The first call of an async generator
    // function fills its allocation profile, and two threads doing that at
    // once trip a known assertion (LANDING-PLAN.md 1.1) unrelated to this test.
    relay(g);

    runOnThreads(s, THREADS, t => {
        (async () => {
            let count = 0;
            for await (const v of (t & 1) ? relay(g) : g) {
                if (typeof v !== "number" || v < 0 || v >= N)
                    bad(s, "for await on thread " + t + " got " + v);
                else
                    Atomics.add(seen, v, 1);
                if (++count % 7 === 0)
                    await null;
            }
            Atomics.add(s, "doneFalse", count);
        })().then(() => {
            Atomics.add(s.settleCounts, t, 1);
            Atomics.add(s, "settled", 1);
        }, error => {
            bad(s, "for await on thread " + t + " threw " + error);
            Atomics.add(s.settleCounts, t, 1);
            Atomics.add(s, "settled", 1);
        });
    });

    checkCommon(s, THREADS, "for await");
    shouldBe(s.yields, N, "for await: the body yielded every value");
    checkDelivered(s, seen, N, "for await");
}
