//@ skip
// (harness library, not a test: loaded by every workload via load(); the
// //@ skip header keeps the corpus runner from executing it standalone once
// the scaling/*.js glob is wired into run-tests.sh — see
// Tools/threads/INTEGRATE-scaling.md.)
//
// JSTests/threads/scaling/harness.js — shared driver for the scaling suite.
//
// The design's success criterion (Pizlo): near-linear scalability running a
// program in parallel with itself, with NO deliberate sharing. Each workload
// in this directory is a self-contained, deterministic function; this harness
// runs N identical, independent copies of it (one per spawned Thread), times
// the spawn-to-join wall clock, and prints one machine-parseable line:
//
//     SCALING <name> <threads> <milliseconds>
//
// Tools/threads/scaling-gate.sh sweeps N in {1,2,4,8}, medians these lines,
// and computes speedup(N) = N * T(1) / T(N) (each thread does the SAME work,
// so total work scales with N; perfect scaling means T(N) == T(1)).
//
// Thread count comes from the harness variable globalThis.SCALING_THREADS,
// injected by the gate script via `jsc -e "globalThis.SCALING_THREADS=N"`.
// Standalone (corpus) runs default to 2 threads flag-on so the parallel path
// is exercised, and to 1 inline run when the Thread global is absent (the
// gate's flag-off serial-identity leg).
//
// Work size scales with globalThis.SCALING_WORK_SCALE so the gate can size
// runs without editing workloads. The gate ALWAYS injects this variable
// explicitly (its --scale flag, default 1, which keeps each gate cell at the
// workloads' nominal ~1s-per-thread release sizing). When the variable is
// ABSENT — i.e. a standalone corpus run via the //@ requireOptions header —
// the default is deliberately FRACTIONAL (CORPUS_DEFAULT_SCALE below):
// a corpus run executes the full workload ~4x effectively serially (two
// warm-up/determinism passes on the calling thread plus 2 spawned threads,
// which serialize under the GIL or heavy contention), and the corpus
// verification ladder also runs under no-JIT and on debug builds. For tight
// interpreted loops the debug-LLInt-vs-release-FTL ratio can be 100-200x,
// not the 20-50x a first draft assumed — and the 2026-06-07 Validate smoke
// (Debug+ASAN, GIL-on) measured allocation-heavy workloads closer to
// 500-1000x, so the margin is now sized from MEASUREMENT, not arithmetic:
// at 1/128 the slowest corpus workload (string-heavy) timed ~24s on that
// build (map-heavy ~18s, splay-like ~14s; raytrace-like additionally had
// its fixed per-frame pixel cost quartered — see its header).
//
// Self-checking: the workload runs twice on the calling thread first (warm-up
// for the JIT tiers AND a determinism check — run 2 must reproduce run 1's
// checksum), then every thread's checksum must equal that reference. Under
// real parallelism (GIL-off) a cross-thread heap corruption that perturbs any
// thread's purely-local computation shows up as a checksum mismatch here,
// independent of timing.
load("../resources/assert.js", "caller relative");

function scalingThreadCount() {
    const n = globalThis.SCALING_THREADS;
    if (typeof n === "number" && n === (n | 0) && n >= 1)
        return n | 0;
    return (typeof Thread === "function") ? 2 : 1;
}

// Standalone (corpus) default when the gate did not inject a scale: small,
// so the ~4 effectively-serial full executions per corpus invocation stay
// bounded under no-JIT / debug slowdowns. The gate passes its own
// SCALING_WORK_SCALE explicitly, so gate measurements never see this.
// 2026-06-07 smoke recalibration: the original 1/32 was sized for ~200x
// slowdown, but the Debug+ASAN GIL-on build measured closer to 500-1000x on
// allocation-heavy workloads (map-heavy 40s, string-heavy 56s at 1/64;
// 18s / 24s at 1/128). 1/128 keeps every workload under ~25s there while
// release builds still get non-trivial work.
const CORPUS_DEFAULT_SCALE = 0.0078125;

function scalingWorkScale() {
    const s = globalThis.SCALING_WORK_SCALE;
    return (typeof s === "number" && s > 0 && s <= 1000) ? s : CORPUS_DEFAULT_SCALE;
}

// Sub-millisecond timing when available (same rationale as bench/harness.js:
// Date.now()'s 1ms quantization is noise the gate's ratios do not need).
const __scalingNowMs = typeof preciseTime === "function"
    ? function() { return preciseTime() * 1000; }
    : Date.now;

function runScalingWorkload(name, workFn) {
    const n = scalingThreadCount();

    // Warm-up + determinism reference on the calling thread.
    const reference = workFn(0);
    shouldBe(workFn(0), reference,
        name + ": workload must be deterministic (warm-up run 2 diverged from run 1)");

    let results;
    let elapsedMs;
    if (typeof Thread === "function") {
        const before = __scalingNowMs();
        const threads = spawnN(n, workFn);
        results = joinAll(threads);
        elapsedMs = __scalingNowMs() - before;
    } else {
        // Flag-off serial-identity leg: no Thread global, so only N=1 makes
        // sense; run the identical work inline so T(1) flag-off is directly
        // comparable to T(1) flag-on.
        if (n !== 1)
            throw new Error(name + ": SCALING_THREADS=" + n + " requires the Thread global (--useJSThreads=1)");
        const before = __scalingNowMs();
        results = [workFn(0)];
        elapsedMs = __scalingNowMs() - before;
    }

    for (let i = 0; i < n; ++i) {
        shouldBe(results[i], reference,
            name + ": thread " + i + " checksum (independent identical work diverged — cross-thread interference)");
    }

    print("SCALING " + name + " " + n + " " + elapsedMs.toFixed(3));

    runSelfTripwire(name, workFn, reference);
}

// Optional in-process re-serialization tripwire, OPT-IN via
//     jsc --useJSThreads=1 -e "globalThis.SCALING_SELF_TRIPWIRE=1;" <workload>
// (default corpus runs leave it off; the full sweep lives in
// Tools/threads/scaling-gate.sh — see Tools/threads/INTEGRATE-scaling.md for
// the pinned --gate rung). This is a cheap single-binary tripwire for GROSS
// re-serialization only: with 2 threads doing identical independent work, a
// reintroduced global lock makes T(2) ~= 2*T(1); a healthy build keeps
// T(2) ~= T(1). Best-of-3 on each side absorbs one-off scheduler spikes, and
// the 1.75x bound leaves wide noise headroom while still catching the ~2x
// serialized case.
//
// MINIMUM-BASELINE GUARD: the ratio argument above only holds when per-thread
// work w dominates per-invocation spawn/join overhead o. At the fractional
// corpus-default work scale (~30ms/thread release) a FULLY serialized build's
// ratio is (o + 2w)/(o + w), which slips under 1.75x for any o > ~w/3 —
// i.e. ~10ms of spawn overhead would make the tripwire pass vacuously on
// exactly the regression it exists to catch (Thread spawn here is heavier
// than a bare OS thread: per-lite VM entry + stack setup). So before
// asserting, the tripwire AMPLIFIES the workload by repetition until the
// measured best-of-3 T(1) is at least TRIPWIRE_MIN_BASELINE_MS; at >= 250ms
// of work, 10-20ms of overhead moves the serialized ratio by < 8%, keeping
// the 2.0x-ideal vs 1.75x-bound margin real. Amplification self-limits: at
// gate scale (or on slow debug builds) one run already exceeds the floor and
// reps stays 1.
const TRIPWIRE_MIN_BASELINE_MS = 250;

function runSelfTripwire(name, workFn, reference) {
    if (!globalThis.SCALING_SELF_TRIPWIRE || typeof Thread !== "function")
        return;
    function bestOf3(fn, nThreads) {
        let best = Infinity;
        for (let r = 0; r < 3; ++r) {
            const before = __scalingNowMs();
            const results = joinAll(spawnN(nThreads, fn));
            const elapsed = __scalingNowMs() - before;
            for (let i = 0; i < nThreads; ++i) {
                shouldBe(results[i], reference,
                    name + ": tripwire thread " + i + " checksum (n=" + nThreads + ")");
            }
            if (elapsed < best)
                best = elapsed;
        }
        return best;
    }
    // Amplify by repetition until T(1) clears the minimum baseline, so the
    // 1.75x assertion below is about WORK, not spawn/join overhead. The loop
    // re-measures after each amplification because reps*time is not exactly
    // linear (warm-up, allocation reuse); the guard bound keeps it finite.
    let reps = 1;
    let fn = workFn;
    let t1 = bestOf3(fn, 1);
    for (let guard = 0; t1 < TRIPWIRE_MIN_BASELINE_MS && guard < 8; ++guard) {
        reps *= Math.max(2, Math.ceil(TRIPWIRE_MIN_BASELINE_MS / Math.max(t1, 1)));
        const myReps = reps;
        fn = function(index) {
            let r;
            for (let k = 0; k < myReps; ++k)
                r = workFn(index);
            return r;
        };
        t1 = bestOf3(fn, 1);
    }
    shouldBeTrue(t1 >= TRIPWIRE_MIN_BASELINE_MS,
        name + ": self-tripwire could not amplify T(1) to >= " + TRIPWIRE_MIN_BASELINE_MS
        + "ms (got " + t1.toFixed(3) + "ms at reps=" + reps
        + ") — refusing to assert a ratio that overhead could fake");
    const t2 = bestOf3(fn, 2);
    print("SCALING-TRIPWIRE " + name + " reps=" + reps
        + " bestT1=" + t1.toFixed(3) + " bestT2=" + t2.toFixed(3));
    shouldBeTrue(t2 < t1 * 1.75,
        name + ": self-tripwire — T(2)=" + t2.toFixed(3) + "ms vs T(1)=" + t1.toFixed(3)
        + "ms exceeds 1.75x; two threads of INDEPENDENT identical work ran like"
        + " one-after-the-other (gross re-serialization: global lock / GIL"
        + " reintroduced on the mutator hot path?)");
}

// WOULD-FAIL-IF: not a test itself, but the failure channel for every
// workload in this suite — runScalingWorkload throws (test FAILs) whenever a
// spawned thread's checksum diverges from the single-thread reference, i.e.
// whenever supposedly-independent per-thread computation was corrupted by
// another mutator (shared-structure/butterfly/heap races); and the SCALING
// line it prints is what scaling-gate.sh's speedup and serial-identity
// checks consume, so a harness regression here silences the whole gate.
// Additionally, with SCALING_SELF_TRIPWIRE=1 injected, a build whose
// 2-thread run of independent work costs ~2x the 1-thread run (gross
// re-serialization) fails the 1.75x tripwire bound without needing the full
// gate sweep — and that claim holds at ANY injected work scale, including
// the fractional corpus default, because the tripwire first amplifies the
// workload by repetition until best-of-3 T(1) >= TRIPWIRE_MIN_BASELINE_MS
// (250ms), so spawn/join overhead cannot drag a fully serialized build's
// (o + 2w)/(o + w) ratio under the bound.
