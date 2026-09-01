//@ requireOptions("--useJSThreads=1")
// MC-TEAR S6 (docs/threads/cve/map-MC-TEAR.md): generator resume frame tear.
// UNGIL-HANDOUT §N.5 (BINDING) requires a single-word resume-claim CAS
// (SuspendedX->Running) and store-RELEASE unclaim transitions in ALL tiers:
// "plain stores torn frames on arm64" — without the release/acquire pairing
// a second resumer pairs the new state word with STALE frame words and
// resumes into a half-written frame (torn {state, frame} publication).
// At audit time @atomicInternalFieldClaim/Publish are NOT in
// builtins/GeneratorPrototype.js — the plain check-then-store remains.
//
// This is the spec's own amplifier shape (§N.5, TSAN AND arm64 hardware):
// two threads ping-pong next() on ONE generator whose body round-trips a
// per-resume counter through frame state; every observed value must be the
// predecessor's published value. Legal outcomes per resume attempt:
//   - {value: k, done: false} where k is exactly nextExpected (serialized
//     winner), or
//   - TypeError (the landed/ruled "generator is already executing" arm for
//     a losing claimant), or
//   - {value: undefined, done: true} only after the generator completes.
// Any other value (skipped counter, repeated counter, garbage) is a torn
// frame. Crashes are the memory-unsafe arm.
//
// WRITTEN DURING BRING-UP: do not execute until the GIL-off ladder is up.
// This test is the acceptance check for landing handout §N.5.
load("../harness.js", "caller relative");

const TOTAL = 4000;

function* counterGen() {
    // Round-trip the counter THROUGH frame state: locals live across yield
    // points, so a torn frame surfaces as a wrong local on resume.
    let a = 0, b = 0, c = 0;
    while (a < TOTAL) {
        a = a + 1;
        b = a * 2;
        c = b - a;       // c === a always, via frame-resident temporaries
        if (c !== a)
            throw new Error("MC-TEAR S6: torn frame inside body: c=" + c
                + " a=" + a);
        yield a;
    }
}

const gen = counterGen();
const box = { done: 0, started: 0 };
// Exactly-once ticket bitmap: tickets[v] flips 0->1 when value v is
// consumed. (Strict consumption ORDER is deliberately not asserted: the
// winner of resume k+1 may ticket before the winner of resume k — that is a
// legal interleaving, not a tear. The generator body itself yields strictly
// increasing values, so duplicates/garbage are the tear signal.)
const tickets = new Uint8Array(new SharedArrayBuffer(TOTAL + 1));

const threads = spawnN(2, (id) => {
    Atomics.add(box, "started", 1);
    waitUntil(() => Atomics.load(box, "started") === 2);
    let mine = 0;
    let typeErrors = 0;
    while (Atomics.load(box, "done") === 0) {
        let r;
        try {
            r = gen.next();
        } catch (e) {
            // Losing claimant: ruled serial arm (§N.5: claim failure on
            // Executing => the existing already-running TypeError; NOT an SD).
            if (e instanceof TypeError) {
                typeErrors++;
                continue;
            }
            throw e;
        }
        if (r.done) {
            Atomics.store(box, "done", 1);
            break;
        }
        // Each yielded value must be consumed exactly once: a failed 0->1
        // CAS means another thread already saw this value => the generator
        // yielded the SAME counter twice => torn/duplicated frame resume.
        const v = r.value;
        if (!Number.isInteger(v) || v < 1 || v > TOTAL)
            throw new Error("MC-TEAR S6: garbage frame value: " + v);
        if (Atomics.compareExchange(tickets, v, 0, 1) !== 0)
            throw new Error("MC-TEAR S6: duplicated resume value " + v
                + " (thread " + id + ")");
        mine++;
    }
    return { mine, typeErrors };
});

const results = joinAll(threads);
// All TOTAL values consumed exactly once across both threads.
let consumed = 0;
for (let v = 1; v <= TOTAL; ++v)
    consumed += tickets[v];
shouldBe(consumed, TOTAL, "every yielded value consumed exactly once");
shouldBe(results[0].mine + results[1].mine, TOTAL,
    "ticket count matches resumes");
// Post-completion behavior: the landed completed arm, never a re-resume.
const after = gen.next();
shouldBeTrue(after.done === true && after.value === undefined,
    "completed generator returns {undefined, true}");
print("mc-tear-generator-resume: PASS (" + results[0].mine + "+"
    + results[1].mine + " resumes, " + results[0].typeErrors + "+"
    + results[1].typeErrors + " serialized TypeErrors)");
