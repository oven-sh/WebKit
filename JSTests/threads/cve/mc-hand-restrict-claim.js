//@ requireOptions("--useJSThreads=1")
// MC-HAND susceptibility test (docs/threads/cve/map-MC-HAND.md, surface S6).
//
// Cancellation / completion / ownership-handoff race on the Thread.restrict
// ownership CLAIM: threadFuncRestrict (ThreadObject.cpp) runs the affinity
// check (step 0) and the table insert (ThreadManager::restrictObject) in two
// SEPARATE m_affinityLock sections, with the 5.7.1 conversion sequence in
// between. The frozen contract is SPEC-api 4.1: "re-restrict from another
// thread => ConcurrentAccessError" — enforced only by step 0. GIL-off, two
// threads can both observe Affinity::None, one wins the insert, and the
// loser's restrictObject takes the live-entry "idempotent re-restrict" arm
// and returns SUCCESS to a thread that is NOT the owner (the arm's comment
// assumes step 0 already rejected foreign callers — atomicity the GIL
// provided and ungil removes). The loser then believes its data is confined
// to itself while the winner retains full access: shared ownership state
// observed by the wrong logical owner — memory-safe data exposure, the
// MC-HAND definition verbatim (ERL-90 / CVE-2025-47907 shape).
//
// Probe: per round, the main thread publishes one fresh plain object; two
// spawned threads race Thread.restrict(o); after a barrier, each thread
// re-probes ownership with a second (non-racing) Thread.restrict(o).
// Legal outcomes per round (4.1):
//   - race phase: exactly ONE "ok" (returns o) and one ConcurrentAccessError;
//   - recheck phase: "ok" iff this thread is the recorded owner (owner
//     idempotency), CAE otherwise;
//   - the recheck winner must be the race winner.
// Susceptibility signals: BOTH racing restricts succeed (phantom ownership
// claim), zero successes, success-reported-but-not-recorded-owner, or any
// non-{o, CAE} outcome. Under the phase-1 GIL each restrict is one atomic
// step, so this passes trivially; post-ungil it is the direct probe of the
// step-0-vs-insert window. Deterministic invariant checking;
// amplifier-ready (the window spans the 5.7.1 conversion sequence, so it is
// wide by MC standards).
load("../harness.js", "caller relative");

const ROUNDS = 200;

const box = { round: 0, obj: null, raced: 0, done: 0 };

function attemptRestrict(o) {
    try {
        const ret = Thread.restrict(o);
        return ret === o ? "ok" : "wrong-return";
    } catch (e) {
        if (e instanceof ConcurrentAccessError)
            return "cae";
        return "error:" + e;
    }
}

function racer() {
    const race = [];
    const recheck = [];
    for (let r = 1; r <= ROUNDS; ++r) {
        waitUntil(() => Atomics.load(box, "round") >= r);
        const o = box.obj; // Plain read, ordered by the seq_cst round load.

        // Race phase: both threads claim ownership of the same fresh object.
        race.push(attemptRestrict(o));
        Atomics.add(box, "raced", 1);

        // Barrier: the table state for this round is settled before either
        // thread re-probes it (no recheck-vs-race interleaving).
        waitUntil(() => Atomics.load(box, "raced") >= 2 * r);

        // Recheck phase (non-racing): 4.1 owner idempotency vs foreign CAE
        // reveals the RECORDED owner deterministically.
        recheck.push(attemptRestrict(o));
        Atomics.add(box, "done", 1);
    }
    return { race, recheck };
}

const t1 = new Thread(racer);
const t2 = new Thread(racer);

for (let r = 1; r <= ROUNDS; ++r) {
    box.obj = { round: r }; // Fresh, never-restricted plain object.
    Atomics.store(box, "round", r); // Release publication of box.obj.
    waitUntil(() => Atomics.load(box, "done") >= 2 * r);
}

const r1 = t1.join();
const r2 = t2.join();
shouldBe(r1.race.length, ROUNDS);
shouldBe(r2.race.length, ROUNDS);

for (let i = 0; i < ROUNDS; ++i) {
    const round = i + 1;
    const raceOutcomes = [r1.race[i], r2.race[i]];
    const recheckOutcomes = [r1.recheck[i], r2.recheck[i]];

    for (const o of raceOutcomes.concat(recheckOutcomes)) {
        if (o !== "ok" && o !== "cae")
            throw new Error("round " + round + ": non-{ok, ConcurrentAccessError} restrict outcome: " + o);
    }

    const raceOks = raceOutcomes.filter(o => o === "ok").length;
    if (raceOks === 0)
        throw new Error("round " + round + ": NO thread won the restrict claim (both got CAE; ownership lost)");
    if (raceOks === 2)
        throw new Error("round " + round + ": BOTH racing Thread.restrict calls succeeded — phantom ownership claim (MC-HAND hit: frozen SPEC-api 4.1 requires CAE for re-restrict from another thread)");

    // Exactly one recorded owner, and it is the thread restrict reported
    // success to. A thread with race "ok" but recheck "cae" was told it owns
    // an object the table assigns to its rival.
    const recheckOks = recheckOutcomes.filter(o => o === "ok").length;
    shouldBe(recheckOks, 1);
    const winnerByRace = r1.race[i] === "ok" ? 1 : 2;
    const winnerByRecheck = r1.recheck[i] === "ok" ? 1 : 2;
    if (winnerByRace !== winnerByRecheck)
        throw new Error("round " + round + ": Thread.restrict reported success to thread " + winnerByRace + " but the affinity table records thread " + winnerByRecheck + " as owner (MC-HAND hit: wrong logical owner holds the confinement claim)");
}
