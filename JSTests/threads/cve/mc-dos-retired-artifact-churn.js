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
// S7 shape (B14, CLOSED — per-thread epoch publication + R2 N-stack scan
// landed): RetiredJITArtifacts::retireHandlerChain / retire() route through
// the epoch facility flag-on (epochCoversEveryJSThread now true), and
// retireOptimizedJITCode releases inline (R2's N-stack scan in
// Heap::gatherStackRoots covers every mutator). The cross-thread arm makes
// the retire rate JS-controllable: foreign touches fire TTL watchpoints,
// jettisoning optimized code; IC megamorphic churn displaces handler
// chains. EXPECTED BEHAVIOR (the S7 leg, now an in-test assertion below):
// process RSS reaches steady state across the churn loop's second half —
// MemoryFootprint().current sampled at the GC checkpoints must not grow
// monotonically past the warm-up midpoint by more than a bounded slop
// (executable-pool fragmentation + unrelated lazy growth). Pre-fix this
// grew unboundedly with ITERS (every displaced chain + every jettisoned
// JITCode leaked); the bounded-RSS assertion is the S7 regression.
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

// S7 leg: bounded-RSS oracle. MemoryFootprint() is the jsc shell's process
// RSS sampler ({current, peak} in bytes). We sample at every GC checkpoint
// after a warm-up half (so tier-up, code installation, and first-touch heap
// growth have stabilized) and require the LAST sample not to exceed the
// first post-warm-up sample by more than a generous slop. The slop absorbs
// (a) executable-pool / malloc fragmentation, (b) the toucher's own
// long-lived allocation, (c) GC eden growth between checkpoints; pre-fix
// the leak grew by tens of MB over the same range (every IC chain + every
// DFG/FTL JITCode for hotRead/hotWrite, ~ITERS× jettisons), so the bound
// discriminates. On platforms without a process-RSS sampler the shell stub
// returns 0 for both fields — the oracle then degenerates to 0<=slop and
// the correctness/completion arms still gate.
const haveFootprint = typeof MemoryFootprint === "function";
const RSS_SLOP_BYTES = 16 * 1024 * 1024;
const WARMUP_ITERS = ITERS >> 1;
let rssBaseline = -1;
let rssLast = -1;

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
        // S7: sample RSS post-gc. Two back-to-back collections so the §11
        // bumpAndReclaim drains items retired BEFORE this checkpoint (an
        // item retired at epoch E needs the NEXT stop's stamp to expire).
        if (haveFootprint && iter >= WARMUP_ITERS) {
            gc();
            const rss = MemoryFootprint().current;
            if (rssBaseline < 0)
                rssBaseline = rss;
            rssLast = rss;
        }
    }
}

Atomics.store(ctl, "stop", 1);
Atomics.store(ctl, "round", ITERS + 1);
Atomics.notify(ctl, "round", Infinity);
const toucherSum = toucher.join();
shouldBeTrue(Number.isInteger(toucherSum), "toucher completed cleanly");

// Final drain: several full collections back-to-back; post-integration this
// must leave the retire backlog empty (RSS steady — asserted below). The
// test's own observable is that nothing crashed, hung, or mis-executed
// across ~ITERS*SHAPES handler-chain retirements and TTL-fire jettisons.
for (let i = 0; i < 4; ++i)
    gc();
shouldBeTrue(true, "retire churn survived " + ITERS + " rounds x " + SHAPES + " shapes");

// S7 leg gate (B14): bounded RSS over the second-half churn. Pre-fix this
// failed by tens of MB (monotone growth — every retire leaked); post-fix
// the epoch drain + R2-licensed JITCode release bound it. rssBaseline < 0
// only when MemoryFootprint is unavailable (stub platform) — the bound
// trivially holds and the run still gates on correctness/completion.
if (haveFootprint && rssBaseline >= 0) {
    const growth = rssLast - rssBaseline;
    shouldBeTrue(growth <= RSS_SLOP_BYTES,
        "S7: RSS bounded over churn second half (baseline=" + rssBaseline
        + " last=" + rssLast + " growth=" + growth
        + " slop=" + RSS_SLOP_BYTES + ")");
}
