//@ requireOptions("--useJSThreads=1")
// MC-PRIM susceptibility test (docs/threads/cve/map-MC-PRIM.md, surface P5).
//
// Trusted-primitive invariant bypass: SPEC-ungil §N.5 makes SYNC generator
// and iterator-helper resume a single-word claim CAS (SuspendedX -> owner
// token via @claimGeneratorResume; landed shape, SPEC-ungil-history "§N.5
// LANDED SHAPE") and then keeps every interior internal-field store PLAIN
// and tier-inlined WHILE CLAIMED - the plain stores are the least-checked
// stores in the resume path, and they trust exactly one construction-time
// invariant: at-most-one-resumer, established by the claim CAS. NOTE: the
// ASYNC generator / async-function resume-head claim is NOT landed (recorded
// §N.5 deferral); that open arm is pinned by
// mc-prim-async-generator-resume-claim.js ([EXPECTED-FAIL GIL-off]), not by
// this test. CVE-2012-0507's shape applies if ANY resume/return/throw/inspection
// path writes generator state without going through the claim (annex N7
// lists the claim+publish sites; the implementation must consume that table
// verbatim). Two threads that both believe they hold the claim interleave
// plain multi-word stores -> torn {state, frame, resumeMode} tuples.
//
// Probe: two spawned threads race .next() on ONE shared generator that
// yields 0..N-1 then finishes. ES semantics admit only two outcomes per
// call: a TypeError ("Generator is executing" - the loser of the claim) or
// a well-formed IteratorResult. Susceptibility signals:
//   - the same value delivered to both threads (duplicate = two resumers
//     both advanced from the same suspended frame);
//   - a skipped value with the generator still claiming completion order
//     (torn state word);
//   - a result that is neither TypeError nor {value, done} (torn
//     publication);
//   - native crash under ASAN/TSAN (torn frame pointer).
// Under the phase-1 GIL each next() is one atomic step, so this passes
// trivially; post-ungil it is the direct probe of the §N.5 claim protocol.
// Deterministic invariant checking; amplifier-ready (the claim window is a
// few instructions - run under the race amplifier for signal).
load("../harness.js", "caller relative");

const N = 4000;

function* makeGen() {
    for (let i = 0; i < N; ++i)
        yield i;
}

const gen = makeGen();
const gate = { go: 0 };

function racer() {
    while (Atomics.load(gate, "go") === 0)
        sleepMs(1);
    const seen = [];
    let typeErrors = 0;
    for (;;) {
        let r;
        try {
            r = gen.next();
        } catch (e) {
            if (!(e instanceof TypeError))
                throw new Error("non-TypeError escaped a racing resume (torn state?): " + e);
            ++typeErrors;
            continue;
        }
        if (typeof r !== "object" || r === null)
            throw new Error("torn IteratorResult publication: " + String(r));
        if (r.done) {
            if (r.value !== undefined)
                throw new Error("completion carried a torn value: " + String(r.value));
            break;
        }
        if (typeof r.value !== "number" || (r.value | 0) !== r.value || r.value < 0 || r.value >= N)
            throw new Error("impossible yielded value (torn resume): " + String(r.value));
        seen.push(r.value);
    }
    return { seen, typeErrors };
}

const t1 = new Thread(racer);
const t2 = new Thread(racer);
Atomics.store(gate, "go", 1);
const r1 = t1.join();
const r2 = t2.join();

// Exactly-once delivery: the union of both threads' values must be a
// duplicate-free subset of 0..N-1, and strictly increasing per thread
// (a single generator never revisits an earlier frame).
for (const r of [r1, r2]) {
    for (let i = 1; i < r.seen.length; ++i) {
        if (r.seen[i] <= r.seen[i - 1])
            throw new Error("per-thread yield order regressed (double resume from one frame): " + r.seen[i - 1] + " then " + r.seen[i]);
    }
}
const all = new Set();
for (const v of r1.seen.concat(r2.seen)) {
    if (all.has(v))
        throw new Error("value " + v + " delivered to BOTH threads: two resumers held the claim (MC-PRIM hit)");
    all.add(v);
}
shouldBe(all.size, r1.seen.length + r2.seen.length);
// Every value 0..N-1 was delivered to exactly one thread.
shouldBe(all.size, N);

// The generator is closed: further next() calls are {undefined, true} on
// any thread, never a resurrection.
const post = gen.next();
shouldBeTrue(post.done === true && post.value === undefined, "generator stays completed");
