//@ requireOptions("--useJSThreads=1", "--maxPerThreadStackUsage=1000000")
// MC-PRIM availability regression test (docs/threads/cve/map-MC-PRIM.md P5 /
// SPEC-ungil §N.5 claim-leak guard).
//
// GIL-off, GeneratorPrototype.js next()/return()/throw() claim the resume
// (CAS SuspendedX -> per-thread token) BEFORE calling @generatorResume, but
// the unclaim/publish used to live only inside @generatorResume (its catch
// arm + epilogue). If the CALL into @generatorResume itself threw before its
// try was entered — deterministically reachable as a stack-overflow
// RangeError from @generatorResume's prologue stack check — the token leaked
// in the State field forever: every later claim on every thread read the
// canonical Executing and the generator threw "Generator is executing"
// permanently (cross-thread object-bricking DoS, and a behavioral divergence
// from GIL-on/flag-off where the same overflow leaves the generator
// resumable).
//
// The landed guard publishes the claim on the throw path of the claiming
// callers (GeneratorPrototype.js + JSIteratorHelperPrototype.js). Publish is
// CAS ourToken -> Completed, so the post-overflow generator is observably
// either RESUMABLE (vanilla-equivalent: the overflow struck before the claim
// or after the unclaim) or CLOSED (the fail-safe publish ran). What it must
// NEVER be is permanently "executing" while no resume is running.
//
// Recorded residual divergence: vanilla leaves the generator resumable after
// a pre-body stack overflow; the GIL-off fail-safe may close it. Accepted —
// the alternative (restoring the observed pre-claim state) needs a third
// host hook for a corner that vanilla programs cannot meaningfully rely on.
load("../harness.js", "caller relative");

function* makeGen() {
    let i = 0;
    while (true)
        yield i++;
}

const gen = makeGen();
shouldBe(gen.next().value, 0); // suspended at the first yield

let sawDeepFailure = false;
function probe() {
    // Recurse until calls start failing, then attempt gen.next() at every
    // unwind depth — one of them lands in the window where next()'s own
    // prologue succeeds but @generatorResume's prologue overflows.
    try {
        probe();
    } catch (e) {
        try {
            gen.next();
        } catch (e2) {
            sawDeepFailure = true;
        }
        throw e;
    }
}
try { probe(); } catch (e) { /* expected RangeError at the root */ }

// Back at top level with the full stack available: the generator must not be
// bricked. Either it resumes (monotone values) or it is closed (done:true);
// "Generator is executing" with no resume running is the leak.
let r;
try {
    r = gen.next();
} catch (e) {
    throw new Error("claim leak: post-overflow resume threw " + e + " (generator permanently bricked)");
}
shouldBeTrue(typeof r === "object" && r !== null, "well-formed IteratorResult after overflow");
if (!r.done) {
    // Resumable arm: values stay monotone and the generator keeps working.
    const next = gen.next();
    shouldBeTrue(typeof next.value === "number" && next.value === r.value + 1, "generator still advances monotonically");
} else {
    // Fail-safe-closed arm: stays closed.
    const post = gen.next();
    shouldBeTrue(post.done === true && post.value === undefined, "closed generator stays closed");
}
