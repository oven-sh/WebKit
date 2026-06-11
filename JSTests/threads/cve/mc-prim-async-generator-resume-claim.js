//@ requireOptions("--useJSThreads=1")
//@ threadsExpectFail("gilOff")
// MC-PRIM / MC-TEAR susceptibility test — ASYNC clone of
// mc-prim-generator-resume-claim.js (docs/threads/cve/map-MC-PRIM.md P5,
// map-MC-TEAR.md S6; annex N7 row R7 names JSAsyncGenerator as §N.5-covered).
//
// [EXPECTED-FAIL GIL-off until the §N.5 ASYNC resume-head claim lands —
// MECHANICAL via the threadsExpectFail("gilOff") directive above: the
// --cve runner counts a GIL-off failure as XFAIL and turns an unexplained
// GIL-off PASS (XPASS) into a suite FAILURE, so this pin cannot rot.]
//
// The landed §N.5 claim/publish protects GeneratorPrototype.js and
// JSIteratorHelperPrototype.js only. AsyncGeneratorPrototype.js still runs
// the plain check-then-store resume head GIL-off (state read
// AsyncGeneratorPrototype.js:35/:82-:83, plain Executing store :78, plain
// queue-field mutations), and JSMicrotask.cpp's C++ resume paths use plain
// setState(Executing) + a plain state re-read to decide done. The deferral is
// recorded (SPEC-ungil-history.md "§N.5 LANDED SHAPE" supersession entry;
// CVE-AUDIT-STATUS.md item 3 amendments). This test pins the open arm
// mechanically: it must FLIP TO PASSING when either (a) claim/publish lands
// on the async resume heads + the JSMicrotask setState cluster, or (b) an
// owner-affinity CAE ruling narrows N7 R7 (in which case the cross-thread
// resume below must surface the CAE, which this test treats as a pass arm).
//
// Probe: two spawned threads race agen.next() on ONE shared async generator
// (synchronous yields — no awaits — so every resume settles on the next
// microtask drain). Susceptibility signals are the sync test's, adapted:
//   - the same value delivered to both threads (two resumers advanced from
//     one suspended frame);
//   - a settled result that is neither a well-formed IteratorResult nor a
//     TypeError/ConcurrentAccessError rejection;
//   - per-thread value order regression;
//   - native crash / debug assert (the :37 Executing assert) — torn
//     {state, frame}.
// Under the phase-1 GIL each drain is one atomic step, so this passes
// trivially; GIL-off it is the direct probe of the missing async claim.
load("../harness.js", "caller relative");

const N = 2000;

async function* makeGen() {
    for (let i = 0; i < N; ++i)
        yield i;
}

const agen = makeGen();
const gate = { go: 0 };

function racer() {
    while (Atomics.load(gate, "go") === 0)
        sleepMs(1);
    const seen = [];
    let rejections = 0;
    let done = false;
    while (!done) {
        let settled = null;
        let failure = null;
        agen.next().then(
            (r) => { settled = r; },
            (e) => { failure = e; });
        drainMicrotasks();
        if (failure !== null) {
            // The claim loser arm: TypeError("Generator is executing") or a
            // ConcurrentAccessError under an owner-affinity ruling.
            if (!(failure instanceof TypeError) && String(failure.name) !== "ConcurrentAccessError")
                throw new Error("non-claim rejection escaped a racing async resume (torn state?): " + failure);
            ++rejections;
            continue;
        }
        if (settled === null) {
            // The resume is parked behind the rival's in-flight resume; the
            // reaction settles on a later drain. Bounded retry.
            let spins = 0;
            while (settled === null && failure === null && spins < 10000) {
                drainMicrotasks();
                sleepMs(0);
                ++spins;
            }
            if (settled === null && failure === null)
                throw new Error("async resume never settled (lost resume / torn queue)");
            if (failure !== null) {
                if (!(failure instanceof TypeError) && String(failure.name) !== "ConcurrentAccessError")
                    throw new Error("non-claim rejection escaped a racing async resume (torn state?): " + failure);
                ++rejections;
                continue;
            }
        }
        if (typeof settled !== "object" || settled === null)
            throw new Error("torn IteratorResult publication: " + String(settled));
        if (settled.done) {
            if (settled.value !== undefined)
                throw new Error("completion carried a torn value: " + String(settled.value));
            done = true;
            break;
        }
        if (typeof settled.value !== "number" || (settled.value | 0) !== settled.value || settled.value < 0 || settled.value >= N)
            throw new Error("impossible yielded value (torn resume): " + String(settled.value));
        seen.push(settled.value);
    }
    return { seen, rejections };
}

const t1 = new Thread(racer);
const t2 = new Thread(racer);
Atomics.store(gate, "go", 1);
const r1 = t1.join();
const r2 = t2.join();

// Exactly-once delivery, strictly increasing per thread (same oracle as the
// sync clone).
for (const r of [r1, r2]) {
    for (let i = 1; i < r.seen.length; ++i) {
        if (r.seen[i] <= r.seen[i - 1])
            throw new Error("per-thread yield order regressed (double resume from one frame): " + r.seen[i - 1] + " then " + r.seen[i]);
    }
}
const all = new Set();
for (const v of r1.seen.concat(r2.seen)) {
    if (all.has(v))
        throw new Error("value " + v + " delivered to BOTH threads: two resumers held the async resume head (MC-PRIM hit)");
    all.add(v);
}
shouldBe(all.size, r1.seen.length + r2.seen.length);
shouldBe(all.size, N);

// The async generator is closed: a further next() settles {undefined, true}.
let post = null;
agen.next().then((r) => { post = r; });
drainMicrotasks();
shouldBeTrue(post !== null && post.done === true && post.value === undefined, "async generator stays completed");
