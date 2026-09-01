# Integrating the scaling suite (JSTests/threads/scaling + Tools/threads/scaling-gate.sh)

Staged under `staging-threads/`. Integration is the `mv` per subtree PLUS one
runner edit — step 2 is a NAMED PRECONDITION, not optional polish (see
"Enforcement channels" below for why):

1. Move the staged trees:

       mv staging-threads/JSTests/threads/scaling  JSTests/threads/scaling
       mv staging-threads/Tools/threads/scaling-gate.sh    Tools/threads/scaling-gate.sh
       mv staging-threads/Tools/threads/INTEGRATE-scaling.md Tools/threads/INTEGRATE-scaling.md

2. Wire the suite into the corpus runner. `Tools/threads/run-tests.sh`
   selects corpus files by EXPLICIT globs (its "collect the corpus" loop:
   `api/ atomics/ races/ heap-*.js objectmodel/ vmstate/` + the `jit/` find);
   without this edit `JSTests/threads/scaling/` is never picked up and the
   suite lands silently dead. Add `"$JT"/scaling/*.js` to that glob list:

       for f in "$JT"/api/*.js "$JT"/atomics/*.js "$JT"/races/*.js \
                "$JT"/heap-*.js "$JT"/objectmodel/*.js "$JT"/vmstate/*.js \
                "$JT"/scaling/*.js; do

   The runner honors `//@` headers: the five workloads and `lock-fairness.js`
   carry `//@ requireOptions("--useJSThreads=1")`, and `harness.js` (a
   library, not a test) carries `//@ skip` so the glob does not execute it
   standalone. Verify with one corpus pass that the scaling files appear in
   the runner's output.

## Enforcement channels — read this before trusting the WOULD-FAIL-IF claims

The five throughput workloads (`splay-like`, `richards-like`, `raytrace-like`,
`string-heavy`, `map-heavy`) each make a TWO-part WOULD-FAIL-IF claim:

1. **Checksum half** (cross-thread corruption of thread-local state): live in
   every execution mode that actually runs these files — gate cells, the
   tripwire, and default corpus runs all compare per-thread checksums against
   a clean serial reference and fail deterministically. **The corpus channel
   exists ONLY after integration step 2 above**: run-tests.sh's globs do not
   include `scaling/`, so until that edit lands the checksum half is dead in
   corpus mode too (the workloads' WOULD-FAIL-IF references to "default
   corpus runs" are conditional on step 2). This is why step 2 is a
   precondition, not a follow-up.

2. **Speedup half** (reintroduced GIL / global allocator lock / serialized IC
   slow path collapsing parallel throughput): live ONLY when the pinned gate
   rung below actually runs. Default corpus runs never compute speedup; the
   gate's default mode is REPORT-ONLY (exit 0) by design for noisy shared
   hosts. **Until the rung below is a named step in the verification ladder,
   this regression class is caught by no execution mode anyone is committed
   to running.** Do not consider this suite integrated until the rung is
   recorded in the ladder (or the staging root's INTEGRATE.md references this
   file as the scaling arm).

## Pinned ladder rung (the scaling gate)

Run on a QUIET machine (the thresholds are meaningless under host
contention), release build, JIT on:

    Tools/threads/scaling-gate.sh --gate WebKitBuild/Release/bin/jsc

Exit 0 = all speedup floors (2.8@4 / 4.5@8; splay-like relaxed to 2.0/3.0
until SPEC-congc), serial identity (±5%), and the lock-fairness smoke met;
the PASS line enumerates exactly which floors were evaluated. `--gate`
refuses (exit 2) a `--threads` sweep that omits 4 or 8 — the only Ns with
defined floors — so a trimmed sweep cannot produce a green that gated
nothing; use report-only mode for partial sweeps. Exit 1 = threshold
violation. Exit 2 = a workload produced a wrong answer (fatal in either
mode), a lock-fairness HARD correctness assert tripped (exclusion overlap /
lost update / accounting — fatal in either mode), or a lock-fairness
STARVATION verdict under `--gate`. In report-only mode a STARVATION verdict
is listed loudly as a would-fail finding but does not exit 2 (liveness
verdicts are timing observations; the shared host is the gate script's own
stated reason report-only exists).

Trend-tracking (report-only, safe on a noisy host — never gates):

    Tools/threads/scaling-gate.sh WebKitBuild/Release/bin/jsc

## Cheap default-mode tripwire (optional, no gate sweep needed)

Gross re-serialization (T(2) ~= 2*T(1) for independent identical work) can be
caught from a single jsc invocation, opt-in:

    WebKitBuild/Release/bin/jsc --useJSThreads=1 \
        -e "globalThis.SCALING_SELF_TRIPWIRE=1;" \
        JSTests/threads/scaling/richards-like.js

This asserts best-of-3 T(2) < 1.75 * best-of-3 T(1) in-process (see
`harness.js`). Before asserting, the tripwire amplifies the workload by
repetition until best-of-3 T(1) >= 250ms, so the bound holds at any injected
work scale — including the fractional corpus default, where ~30ms of work
would otherwise let per-spawn overhead drag a FULLY serialized build's
(o + 2w)/(o + w) ratio under 1.75x (a vacuous pass). It is NOT a substitute
for the gate rung — it cannot see partial scaling collapse (e.g.
speedup(8) = 2x) — but it does fail on a reintroduced global lock without
any harness wiring.

## Validate-phase obligation (carried from harness.js)

DISCHARGED 2026-06-07: the Validate smoke (Debug+ASAN GIL-on build) measured
the original CORPUS_DEFAULT_SCALE = 0.03125 sizing as 2-4x over budget
(map-heavy/string-heavy/raytrace-like all exceeded a 60s timeout; the real
debug/ASAN slowdown on allocation-heavy code is ~500-1000x, not the assumed
200x). CORPUS_DEFAULT_SCALE is now 0.0078125 (1/128) and raytrace-like's
fixed per-frame cost was quartered (96x96 -> 48x48 pixels). Measured on that
build at the new sizing: splay-like ~14s, map-heavy ~18s, string-heavy ~24s,
raytrace-like see staging INTEGRATE.md smoke table; richards-like ~5s and
lock-fairness ~8s were already within budget.
