//@ requireOptions("--useJSThreads=1", "--thresholdForJITAfterWarmUp=20", "--thresholdForOptimizeAfterWarmUp=100", "--thresholdForFTLOptimizeAfterWarmUp=200")
// ftl-osr-entry-catch-loop-amplifier.js — UNGIL U-T4b amplifier: concurrent
// catch / loop OSR entry on ONE CodeBlock (SPEC-ungil §A.1.6 / ANNEX A16).
//
// gilOff, the JITCode-RESIDENT OSR buffers (DFG/FTL catchOSREntryBuffer, FTL
// ForOSREntry m_entryBuffer) become per-lite registry indices: each entering
// thread fills and reads back ITS OWN lite's buffer at the baked index. This
// test makes N threads hammer the SAME two functions:
//
//   catchy(): throws from a hot loop into a catch block whose body is itself
//   hot — the op_catch entrypoint is compiled, so every throw drives
//   prepareCatchOSREntry (host fill) + ExtractCatchLocal/ClearCatchLocals
//   (emitted readback) on the shared CodeBlock. A torn shared buffer shows up
//   as another thread's locals (wrong seed-derived values) or a crash.
//
//   hot(): a long-running loop that tiers up mid-execution (loop OSR entry,
//   DFG then FTL ForOSREntry) — concurrent entries on one entry CodeBlock
//   exercise the entry-buffer fill (FTLOSREntry.cpp) vs readback
//   (ExtractOSREntryLocal) per-lite split.
//
// Phase-1 (GIL'd) the test still passes — it asserts pure values, not
// interleavings; the amplification only bites once mutators truly overlap.

load("../harness.js", "caller relative");

const THREADS = 4;
const ROUNDS = 8;

function hot(seed, n) {
    let a = seed | 0, b = (seed ^ 0x5bf03635) | 0, c = 0;
    for (let i = 0; i < n; ++i) {
        a = (a + i) | 0;
        b = (b ^ a) | 0;
        c = (c + (a & 0xffff) - (b & 0xff)) | 0;
    }
    return (a ^ b ^ c) | 0;
}
noInline(hot);

function catchy(seed, n) {
    let acc = seed | 0, mark = 0, spill1 = (seed * 3) | 0, spill2 = (seed * 7) | 0;
    try {
        for (let i = 0; i < n; ++i) {
            acc = (acc + i) | 0;
            spill1 = (spill1 ^ acc) | 0;
            if (i === (n - 3))
                throw acc;
        }
    } catch (e) {
        // Hot catch body => op_catch entrypoint compiles; locals (acc, spill1,
        // spill2) are reconstructed through the catch OSR entry buffer.
        mark = e | 0;
        for (let j = 0; j < 5000; ++j)
            mark = (mark + ((spill1 ^ spill2) & 0xff) + j) | 0;
    }
    return (mark ^ spill2) | 0;
}
noInline(catchy);

// Expected values per seed, computed single-threaded BEFORE spawning (same
// CodeBlocks, pre-warmed so spawned threads hit compiled tiers immediately).
const HOT_N = 60000;
const CATCH_N = 64;
const expectedHot = [];
const expectedCatchy = [];
for (let t = 0; t < THREADS; ++t) {
    expectedHot[t] = hot(t + 1, HOT_N);
    expectedCatchy[t] = catchy(t + 1, CATCH_N);
}
// Extra single-threaded warmup to push both through the tiers (loop OSR
// entry triggers inside hot() itself thanks to the lowered thresholds).
for (let w = 0; w < 50; ++w) {
    hot(99, HOT_N >> 4);
    catchy(99, CATCH_N);
}

const failures = { count: 0 };

const threads = spawnN(THREADS, function (t) {
    let bad = 0;
    for (let r = 0; r < ROUNDS; ++r) {
        if (hot(t + 1, HOT_N) !== expectedHot[t])
            ++bad;
        for (let k = 0; k < 25; ++k) {
            if (catchy(t + 1, CATCH_N) !== expectedCatchy[t])
                ++bad;
        }
    }
    return bad;
});

const results = joinAll(threads);
for (let t = 0; t < THREADS; ++t)
    shouldBe(results[t], 0, "thread " + t + " observed torn OSR-entry locals");

// Main thread interleaves the same CodeBlocks while children run is covered
// by the warmup above plus this post-join re-check (catch + loop entry still
// sane after concurrent use).
shouldBe(hot(1, HOT_N), expectedHot[0]);
shouldBe(catchy(1, CATCH_N), expectedCatchy[0]);
