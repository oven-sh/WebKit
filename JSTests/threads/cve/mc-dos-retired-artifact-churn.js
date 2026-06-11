//@ requireOptions("--useJSThreads=1", "--useVMLite=1", "--useSharedAtomStringTable=1", "--useSharedGCHeap=1", "--useThreadGILOffUnsafe=1")
// MC-DOS S6+S7 (docs/threads/cve/map-MC-DOS.md): sustained retire-side
// pressure on the epoch facility and the retired-JIT-artifact paths.
//
// S6 shape: GCSafepointEpoch::m_retired is an unbounded Vector reclaimed
// ONLY inside a collection (SPEC-heap §11 / I11); retired bytes are not
// reported to GC heuristics, so a workload with a high retire rate and a
// low JS allocation rate grows native memory with nothing pushing the
// collector. This test IS that workload — IC handler-chain churn on
// long-lived objects — with explicit gc() interleaved so each shared
// collection gets its reclaim opportunity; post-integration, survival at
// steady state is the regression that bumpAndReclaim actually drains the
// backlog (and that the I10 no-op exemption doesn't silently skip it).
//
// S7 shape (CHARTERED, leak-until-integration): flag-on,
// RetiredJITArtifacts::retireHandlerChain and retireOptimizedJITCode leak
// unconditionally (epochCoversEveryJSThread returns !useJSThreads —
// bytecode/RetiredJITArtifacts.cpp). The cross-thread arm makes the leak
// rate JS-controllable: foreign touches fire TTL watchpoints, jettisoning
// optimized code; IC megamorphic churn displaces handler chains. EXPECTED
// BEHAVIOR per landing state:
//   - while the charter is open: this test still PASSES functionally, but
//     RSS / executable-pool consumption grows monotonically with ITERS —
//     run it under an external RSS/pool monitor (the harness assertion
//     here is correctness + completion, the quota assertion is external);
//   - after per-thread epoch publication + the R2 N-stack scan land:
//     memory must reach steady state; re-run under the monitor and keep
//     this test as the regression for BOTH landings.
//
// Deterministic in outcome; amplifier-ready (ITERS/SHAPES are the knobs;
// rule 1: never shrink them to call the surface safe).
load("../harness.js", "caller relative");

const ITERS = 400;        // outer churn rounds
const SHAPES = 24;        // > megamorphic threshold: forces chain churn
const GC_EVERY = 25;      // explicit reclaim opportunities (S6)

// Long-lived shape zoo: each round feeds every shape through hot get/put
// sites, then perturbs shapes so ICs reset and displaced handler chains
// are retired. Objects are long-lived on purpose (low allocation rate,
// high retire rate — the S6 decoupling).
const zoo = [];
for (let s = 0; s < SHAPES; ++s) {
    const o = { tag: s };
    o["p" + s] = s;          // distinct structure per element
    o.shared = s * 3;
    zoo.push(o);
}

function hotRead(o) { return o.shared; }          // megamorphic get site
function hotWrite(o, v) { o.shared = v; }         // megamorphic put site
noInline(hotRead);
noInline(hotWrite);

// Cross-thread arm: a spawned thread repeatedly touches the SAME zoo
// (foreign reads + foreign transitions), firing TTL watchpoints and
// jettisoning whatever optimized code the main thread tiered up — the
// retireOptimizedJITCode leg (S7). It checkpoints through a shared cell.
const ctl = { round: 0, stop: 0, sum: 0 };
const toucher = new Thread((zoo, ctl) => {
    let localSum = 0;
    let seenRound = 0;
    while (!Atomics.load(ctl, "stop")) {
        const r = Atomics.load(ctl, "round");
        if (r === seenRound) {
            Atomics.wait(ctl, "round", r, 50); // park until main advances
            continue;
        }
        seenRound = r;
        for (const o of zoo) {
            localSum += o.shared | 0;          // foreign read
            o["foreign" + (r & 7)] = r;        // foreign transition: TTL fire
        }
        Atomics.store(ctl, "sum", localSum | 0);
    }
    return localSum | 0;
}, zoo, ctl);

let expected = 0;
for (let iter = 1; iter <= ITERS; ++iter) {
    // Hot megamorphic traffic: tiers up, builds handler chains.
    let sum = 0;
    for (let inner = 0; inner < 50; ++inner) {
        for (let s = 0; s < SHAPES; ++s) {
            hotWrite(zoo[s], iter + s);
            sum += hotRead(zoo[s]);
        }
    }
    expected = sum;

    // Shape perturbation: delete + re-add on a rotating victim resets its
    // ICs; displaced chains hit retireHandlerChain.
    const victim = zoo[iter % SHAPES];
    delete victim.shared;
    victim.shared = iter % SHAPES; // value restored next round by hotWrite

    // Advance the toucher: foreign transitions over the whole zoo (TTL
    // fires -> jettison -> retireOptimizedJITCode).
    Atomics.store(ctl, "round", iter);
    Atomics.notify(ctl, "round", Infinity);

    if (iter % GC_EVERY === 0) {
        gc(); // S6: every shared collection is a reclaim license (§10 step 7)
        // Correctness through the churn: the hot read must still see the
        // values the hot write published this round.
        const check = 50 * SHAPES * iter + 50 * (SHAPES * (SHAPES - 1) / 2);
        shouldBe(expected, check, "iter " + iter + ": megamorphic IC stayed correct through retire churn");
    }
}

Atomics.store(ctl, "stop", 1);
Atomics.store(ctl, "round", ITERS + 1);
Atomics.notify(ctl, "round", Infinity);
const toucherSum = toucher.join();
shouldBeTrue(Number.isInteger(toucherSum), "toucher completed cleanly");

// Final drain: several full collections back-to-back; post-integration this
// must leave the retire backlog empty (externally: RSS steady). The test's
// own observable is that nothing crashed, hung, or mis-executed across
// ~ITERS*SHAPES handler-chain retirements and TTL-fire jettisons.
for (let i = 0; i < 4; ++i)
    gc();
shouldBeTrue(true, "retire churn survived " + ITERS + " rounds x " + SHAPES + " shapes");
