//@ requireOptions("--useJSThreads=1")
// semantics/regexp-lastindex-shared.js — two threads exec() the SAME global
// regexp object on the same subject. `lastIndex` is ordinary shared mutable
// state on the shared RegExp object (per SPEC semantics it is just a
// property), so concurrent exec loops RACE on it by design.
//
// Documented racy-but-memory-safe semantics asserted here:
//   - no crash, no hang (bounded loops, every thread joined);
//   - every value ever observed in re.lastIndex is a value some exec() (or
//     the initial state) legitimately wrote: a member of the precomputed
//     valid set {0, end-of-match-1, end-of-match-2, ...} — never a torn
//     number, never garbage, never out of [0, subject.length];
//   - every non-null match is bit-exact one of the real matches of the
//     pattern in the subject (index + captured text agree with the
//     single-threaded oracle); a null result is legal whenever some write
//     left lastIndex past the final match (exec then resets it to 0);
//   - what is NOT asserted: which match a given exec returns (interleaving
//     decides that — that's the racy part, and it is allowed).
// A second, lock-protected section then shows the deterministic semantics:
// under a Lock the two threads consume the match sequence exactly.
load("../harness.js", "caller relative");

const subject = "xx_ab_xx_abb_xx_abbb_xx_ab_xx_abbbb_done";

// Single-threaded oracle: full match list + the set of legal lastIndex
// values, computed on a private regexp before any thread exists.
const oracle = [];
const validLastIndex = { 0: true };
{
    const re = /ab+/g;
    let m;
    while ((m = re.exec(subject))) {
        oracle.push({ index: m.index, text: m[0] });
        validLastIndex[m.index + m[0].length] = true;
    }
}
shouldBe(oracle.length, 5, "oracle match count"); // ab, abb, abbb, ab, abbbb
const oracleByIndex = {};
for (const m of oracle)
    oracleByIndex[m.index] = m.text;

// --- Racy section: shared regexp, no lock. ---
const sharedRe = /ab+/g;
const gate = { ready: 0, go: 0 };
const ROUNDS = 3000;

const racers = spawnN(2, t => {
    Atomics.add(gate, "ready", 1);
    while (Atomics.load(gate, "go") === 0)
        Atomics.wait(gate, "go", 0, 2);
    let nulls = 0, hits = 0;
    for (let r = 0; r < ROUNDS; ++r) {
        // Raw read of the shared lastIndex: must always be a legitimately
        // written value (or a number a concurrent exec wrote — same set).
        const li = sharedRe.lastIndex;
        if (!(typeof li === "number" && validLastIndex[li] === true))
            throw new Error("thread " + t + " round " + r + ": lastIndex held a never-written value: " + li);
        const m = sharedRe.exec(subject);
        if (m === null) {
            ++nulls;
            // Spec: failed exec on a global regexp resets lastIndex to 0.
            // (A concurrent exec may overwrite it before we look — so no
            // assert on the post-state; the read at loop top covers it.)
        } else {
            ++hits;
            const expectText = oracleByIndex[m.index];
            if (expectText === undefined)
                throw new Error("thread " + t + " round " + r + ": match at impossible index " + m.index);
            if (m[0] !== expectText)
                throw new Error("thread " + t + " round " + r + ": torn match at " + m.index + ": " + m[0] + " vs " + expectText);
        }
        if ((r & 255) === 255)
            sleepMs(1); // GIL-dropping yield so the peer interleaves
    }
    return hits + ":" + nulls;
});

waitUntil(() => Atomics.load(gate, "ready") === 2);
Atomics.store(gate, "go", 1);
Atomics.notify(gate, "go", Infinity);
const tallies = joinAll(racers);
for (let t = 0; t < 2; ++t) {
    const [hits, nulls] = tallies[t].split(":").map(Number);
    shouldBe(hits + nulls, ROUNDS, "thread " + t + " completed all rounds");
    shouldBeTrue(hits >= 1, "thread " + t + " matched at least once");
}

// --- Deterministic section: same shared regexp under a Lock. ---
sharedRe.lastIndex = 0;
const lock = new Lock();
const log = { items: [], done: 0 };
// Bounded: a correct serialized walk needs at most oracle.length+1 execs
// total, so oracle.length+2 iterations PER consumer is generous slack. If
// the loop bound is exhausted without seeing the terminating null, the
// serialized exec is not advancing lastIndex (e.g. lastIndex writes lost /
// not published under the Lock — the test must fail loudly here, not spin
// until the harness timeout).
const MAX_LOCKED_ITERS = oracle.length + 2;
const consumers = spawnN(2, t => {
    for (let iter = 0; iter < MAX_LOCKED_ITERS; ++iter) {
        let finished = false;
        lock.hold(() => {
            if (log.done) {
                // The peer already observed the null: stop WITHOUT exec'ing.
                // The failed exec reset lastIndex to 0, so one more exec here
                // would re-walk the whole subject and duplicate every match.
                finished = true;
                return;
            }
            const m = sharedRe.exec(subject);
            if (m === null) {
                log.done = 1; // shared stop: both consumers terminate
                finished = true;
                return;
            }
            // Push inside the SAME critical section as the exec so the log
            // order is exactly the serialized exec order.
            log.items.push(m.index + "=" + m[0]);
            // Fail on the FIRST surplus match instead of looping: more
            // pushes than oracle matches means the serialized walk is
            // repeating itself (lastIndex not advancing/publishing).
            if (log.items.length > oracle.length)
                throw new Error("locked exec produced a surplus match (lastIndex not advancing under the Lock): consumed="
                    + log.items.length + " last=" + log.items[log.items.length - 1]);
        });
        if (finished)
            return true;
    }
    throw new Error("locked consumer " + t + " never reached the terminating null in "
        + MAX_LOCKED_ITERS + " serialized execs: consumed=" + log.items.length
        + " items=[" + log.items.join(", ") + "] lastIndex=" + sharedRe.lastIndex);
});
joinAll(consumers);
// Locked consumption walks the match sequence exactly once, in order. The
// shared done flag is essential: only one thread observes the first null,
// and without the flag the surviving thread's next exec would start from
// the reset lastIndex (0) and deterministically re-consume all 4 matches.
shouldBe(log.items.length, oracle.length, "locked section consumed each match exactly once");
for (let i = 0; i < oracle.length; ++i)
    shouldBe(log.items[i], oracle[i].index + "=" + oracle[i].text, "locked match " + i + " in order");

// WOULD-FAIL-IF: shared-regexp execution stops being memory-safe under
// concurrent exec: a torn or out-of-thin-air lastIndex (e.g. the matcher
// keeps a raw pointer/offset computed from one thread's lastIndex write
// while another's exec frees or moves the subject/ovector scratch — the
// AUD1.N2 RegExp-ovector-per-lite family), a match result assembled from
// another thread's ovector (match text disagreeing with the oracle at that
// index), or a crash/hang in Yarr when two threads run the same compiled
// pattern. The valid-set check on every raw lastIndex read and the oracle
// compare on every match trip on any non-linearizable value; the locked
// section additionally fails if Lock no longer makes regexp state exactly
// sequential.
