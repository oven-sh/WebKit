//@ requireOptions("--useJSThreads=1")
// semantics/async-generator-multithread-requests.js — several threads call
// next(), return() and throw() on ONE async generator.
//
// The requests are linearized by the engine (GIL-off: the generator's cell
// lock, see JSAsyncGenerator.cpp), so no order is asserted. What must hold
// under any interleaving:
//   - every request's promise settles, exactly once;
//   - each yielded value reaches exactly one request, and the delivered values
//     are exactly the ones the body yielded (no duplicate, none lost);
//   - a request made after a return()/throw() that completed the generator was
//     observed to settle gets {value: undefined, done: true} (or, for throw(),
//     a rejection);
//   - the body never runs on two threads at once (an Atomics counter on entry
//     and exit of every stretch of body code between suspensions).
//
// GIL-off a promise reaction runs on the thread that settled the promise, and
// an await inside the body resumes on the thread that resolved the awaited
// promise. So every thread, main included, drains its own microtask queue
// until a shared count says all requests have settled. A request that is
// queued and never run shows up as that count not being reached by the
// deadline.
load("../harness.js", "caller relative");

const THREADS = 4;
const DEADLINE_MS = 60000;

function makeShared(total) {
    return {
        lock: new Lock(),
        inBody: 0,
        overlap: 0,
        yields: 0,
        caught: 0,
        returns: 0,
        throws: 0,
        throwsFulfilled: 0,
        started: 0,
        finished: 0,
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

// Drains until request idx has settled. A thread that waits like this often
// finds the generator idle at its next request, and so drives it: the driver
// changes threads often.
function waitForRequest(s, idx) {
    const deadline = Date.now() + DEADLINE_MS;
    for (let spins = 1; Atomics.load(s.settleCounts, idx) === 0; ++spins) {
        drainMicrotasks();
        poke(s);
        if (Date.now() > deadline)
            throw new Error("request " + idx + " never settled");
        if (!(spins % 64))
            sleepMs(1);
    }
}

// Records one settlement of request idx, and passes the result to onFulfilled
// or onRejected.
function track(s, idx, promise, onFulfilled, onRejected) {
    const settledOnce = () => {
        if (Atomics.add(s.settleCounts, idx, 1) !== 0)
            bad(s, "request " + idx + " settled twice");
        Atomics.add(s, "settled", 1);
    };
    promise.then(result => {
        try {
            onFulfilled(result);
        } catch (e) {
            bad(s, "request " + idx + ": " + e);
        }
        settledOnce();
    }, error => {
        try {
            onRejected(error);
        } catch (e) {
            bad(s, "request " + idx + ": " + e);
        }
        settledOnce();
    });
}

// A next() result. finishedBefore is whether this thread had seen the
// generator completed before it made the request.
function checkNextResult(s, seen, result, finishedBefore, what) {
    if (result.done) {
        if (result.value !== undefined)
            throw new Error(what + " done with value " + result.value);
        Atomics.store(s, "finished", 1);
        return;
    }
    if (finishedBefore)
        throw new Error(what + " got value " + result.value + " after the generator was seen completed");
    if (typeof result.value !== "number" || result.value < 0 || result.value >= seen.length)
        throw new Error(what + " got unexpected value " + result.value);
    Atomics.add(seen, result.value, 1);
    Atomics.add(s, "doneFalse", 1);
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

// Like countingBody, but catches what throw() delivers at a yield and goes on.
async function* catchingBody(s, n) {
    enter(s);
    try {
        for (let i = 0; i < n; ++i) {
            if (i % 5 === 2) {
                leave(s);
                try { await crossThreadTick(s); } finally { enter(s); }
            }
            Atomics.add(s, "yields", 1);
            let thrown = false;
            leave(s);
            try { yield i; } catch { thrown = true; } finally { enter(s); }
            if (thrown)
                Atomics.add(s, "caught", 1);
        }
    } finally {
        leave(s);
    }
}

// Yields 0 once.
async function* once(s) {
    enter(s);
    Atomics.add(s, "started", 1);
    try {
        Atomics.add(s, "yields", 1);
        leave(s);
        try { yield 0; } finally { enter(s); }
    } finally {
        leave(s);
    }
}

// GIL-off, a thread can run code that was compiled while a function had one
// activation, after a second activation exists, and read the first
// activation's variables (docs/threads/REVIEW-async-generator.md). A body that
// read an earlier section's shared object would hang this test. That bug is
// not in the generator code, so make a second activation of each body now.
countingBody(makeShared(1), 0).next();
catchingBody(makeShared(1), 0).next();
once(makeShared(1)).next();
drainMicrotasks();

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

// The kind of request j of thread t in the mixed runs: next() for the first
// third, so the body runs for a while with requests from every thread, then
// mostly next() with an occasional return() or throw().
function kindOf(t, j, per, rareEvery) {
    if (j < per / 3)
        return "next";
    let h = Math.imul(t + 1, 0x9e3779b1) ^ Math.imul(j + 1, 0x85ebca6b);
    h = Math.imul(h ^ (h >>> 15), 0xc2b2ae35);
    const r = ((h ^ (h >>> 13)) >>> 0) % rareEvery;
    return r === 0 ? "return" : r === 1 ? "throw" : "next";
}

// --- 1. next() only. More requests than values, so both a value for every
// request until the body ends, and {done: true} after it.
{
    const PER = 60;
    const N = THREADS * PER - 40;
    const total = THREADS * PER;
    const s = makeShared(total);
    const seen = new Int32Array(new SharedArrayBuffer(4 * N));
    const g = countingBody(s, N);

    runOnThreads(s, total, t => {
        for (let j = 0; j < PER; ++j) {
            const idx = t * PER + j;
            track(s, idx, g.next(), result => checkNextResult(s, seen, result, false, "next " + idx),
                error => { throw new Error("next " + idx + " rejected: " + error); });
            // Thread 0 sends bursts; the others wait for each request.
            if (t)
                waitForRequest(s, idx);
            else if (j % 8 === 7)
                drainMicrotasks();
        }
    });

    checkCommon(s, total, "next only");
    shouldBe(s.yields, N, "next only: the body yielded every value");
    checkDelivered(s, seen, N, "next only");
}

// --- 2. next(), return() and throw() mixed, on a body that does not catch. The
// first return() or throw() to reach a yield completes the generator.
{
    const PER = 60;
    const N = 1000;
    const total = THREADS * PER;
    const s = makeShared(total);
    const seen = new Int32Array(new SharedArrayBuffer(4 * N));
    const g = countingBody(s, N);

    runOnThreads(s, total, t => {
        for (let j = 0; j < PER; ++j) {
            const idx = t * PER + j;
            const finishedBefore = Atomics.load(s, "finished") === 1;
            const kind = kindOf(t, j, PER, 24);
            if (kind === "return") {
                Atomics.add(s, "returns", 1);
                const tag = 100000 + idx;
                track(s, idx, g.return(tag), result => {
                    if (result.value !== tag || result.done !== true)
                        throw new Error("return " + idx + " got {" + result.value + ", " + result.done + "}");
                    Atomics.store(s, "finished", 1);
                }, error => { throw new Error("return " + idx + " rejected: " + error); });
            } else if (kind === "throw") {
                Atomics.add(s, "throws", 1);
                const tag = 200000 + idx;
                track(s, idx, g.throw(tag), result => {
                    throw new Error("throw " + idx + " fulfilled with {" + result.value + ", " + result.done + "}");
                }, error => {
                    if (error !== tag)
                        throw new Error("throw " + idx + " rejected with " + error);
                    Atomics.store(s, "finished", 1);
                });
            } else {
                track(s, idx, g.next(), result => checkNextResult(s, seen, result, finishedBefore, "next " + idx),
                    error => { throw new Error("next " + idx + " rejected: " + error); });
            }
            // Thread 0 sends bursts; the others wait for each request.
            if (t)
                waitForRequest(s, idx);
            else if (j % 6 === 5)
                drainMicrotasks();
        }
    });

    checkCommon(s, total, "mixed");
    shouldBeTrue(s.returns + s.throws > 0, "mixed: the run made a return() or throw()");
    shouldBeTrue(s.yields < N, "mixed: a return() or throw() ended the body");
    checkDelivered(s, seen, s.yields, "mixed");
}

// --- 3. next() and throw() on a body that catches. A caught throw() gets the
// next yielded value, so values go to both kinds.
{
    const PER = 60;
    const N = THREADS * PER - 50;
    const total = THREADS * PER + 1;
    const s = makeShared(total);
    const seen = new Int32Array(new SharedArrayBuffer(4 * N));
    const g = catchingBody(s, N);

    // Start the body on main, so a throw() finds it at a yield rather than
    // suspended-start (which would complete it).
    track(s, total - 1, g.next(), result => checkNextResult(s, seen, result, false, "first next"),
        error => { throw new Error("first next rejected: " + error); });

    runOnThreads(s, total, t => {
        for (let j = 0; j < PER; ++j) {
            const idx = t * PER + j;
            const finishedBefore = Atomics.load(s, "finished") === 1;
            if (kindOf(t, j, PER, 4) === "next") {
                track(s, idx, g.next(), result => checkNextResult(s, seen, result, finishedBefore, "next " + idx),
                    error => { throw new Error("next " + idx + " rejected: " + error); });
            } else {
                const tag = 300000 + idx;
                track(s, idx, g.throw(tag), result => {
                    Atomics.add(s, "throwsFulfilled", 1);
                    checkNextResult(s, seen, result, finishedBefore, "throw " + idx);
                }, error => {
                    if (error !== tag)
                        throw new Error("throw " + idx + " rejected with " + error);
                    // The body catches every throw() delivered at a yield, so a
                    // rejection means the generator had completed.
                    Atomics.store(s, "finished", 1);
                });
            }
            // Thread 0 sends bursts; the others wait for each request.
            if (t)
                waitForRequest(s, idx);
            else if (j % 8 === 7)
                drainMicrotasks();
        }
    });

    checkCommon(s, total, "catching");
    shouldBe(s.yields, N, "catching: the body yielded every value");
    checkDelivered(s, seen, N, "catching");
    shouldBe(s.caught, s.throwsFulfilled, "catching: caught throws and fulfilled throw() requests");
}

// --- 4. One request per thread on a generator that has not started, all made
// at once. Whichever request is linearized first decides: a next() starts the
// body, which yields 0 to it; a return() or throw() completes the generator
// before the body runs.
{
    const ROUNDS = 30;
    const T = 3;

    for (let round = 0; round < ROUNDS; ++round) {
        const s = makeShared(T);
        const seen = new Int32Array(new SharedArrayBuffer(4));
        const g = once(s);
        const gate = { ready: 0, go: 0 };

        const threads = spawnN(T, t => {
            Atomics.add(gate, "ready", 1);
            while (Atomics.load(gate, "go") === 0)
                Atomics.wait(gate, "go", 0, 20);
            const kind = ["next", "return", "throw"][(t + round) % 3];
            if (kind === "next") {
                track(s, t, g.next(), result => checkNextResult(s, seen, result, false, "next"),
                    error => { throw new Error("next rejected: " + error); });
            } else if (kind === "return") {
                track(s, t, g.return(7), result => {
                    if (result.value !== 7 || result.done !== true)
                        throw new Error("return got {" + result.value + ", " + result.done + "}");
                }, error => { throw new Error("return rejected: " + error); });
            } else {
                track(s, t, g.throw(8), result => {
                    throw new Error("throw fulfilled with {" + result.value + ", " + result.done + "}");
                }, error => {
                    if (error !== 8)
                        throw new Error("throw rejected with " + error);
                });
            }
            drainUntilSettled(s, T);
            return t;
        });
        waitUntil(() => Atomics.load(gate, "ready") === T, DEADLINE_MS, 1);
        Atomics.store(gate, "go", 1);
        Atomics.notify(gate, "go");
        drainUntilSettled(s, T);
        joinAll(threads);
        drainMicrotasks();

        const label = "not started, round " + round;
        checkCommon(s, T, label);
        shouldBeTrue(s.started === 0 || s.started === 1, label + ": body started " + s.started + " times");
        checkDelivered(s, seen, s.started, label);
    }
}
