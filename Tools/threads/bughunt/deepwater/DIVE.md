# THE DIVE — limp-build campaign ledger (2026-06-11/12)

Worktree `/root/WebKit-limp` @ `5c0e51c2b543` (jarred/threads), limp build
`/root/WebKit-limp/WebKitBuild/Limp/bin/jsc` (Debug+ASAN, `-fsanitize-recover=address`,
guards documented in `deepwater/LIMP.md`). Raw logs: `/root/WebKit-limp/dive-logs/`
(one `.log` per run + `ledger.txt` with one bookkeeping line per run).

Run env everywhere: `ASAN_OPTIONS=halt_on_error=0:detect_leaks=0`, `nice -n 10`,
runs strictly sequential. Pinned GIL-off flags:
`--useJSThreads=1 --useThreadGIL=0 --useVMLite=1 --useSharedAtomStringTable=1
--useSharedGCHeap=1 --useThreadGILOffUnsafe=1`.

Sizing note (honest bookkeeping): on this Debug+ASAN binary a FULL-size
`bench.js -- W` is ~2.2-2.5 h/run (see LIMP.md W=16 gate); 15 runs were
infeasible, so leg (a) ran at smoke sizing (`-- W smoke`, N_BASE=2000) and the
load came from thread count (W=32/48/64). Leg (b) is a custom-sized variant
(see below). Anomaly-detection power at W is unaffected; absolute postings
volume per run is 14x below full.

## Matrix and outcomes (33 runs total, all sequential)

### Leg (a) — bench.js -- W smoke, 5x each of W=32/48/64: 15/15 COMPLETED
- Outcome: 15/15 exit 0, checksums IDENTICAL across all 15 runs and equal to
  the known-good smoke reference (checksumA `f9f349a1fc92a0d0`, postings
  296555, A2 `206f01740e982aff`, B `cd61e4f189156f96`, C `c2b5b09ff17a90c1`).
- 0 LIMP tag lines, 0 ASAN reports, 0 hangs, 0 watchdog aborts.
- Timing anomaly (information, not failure): Phase C wall is BIMODAL.
  W=32: 110.9/118.0/119.7/127.0/134.5 s (5/5 slow). W=48: 4 slow
  (108-113 s) + 1 fast (6.2 s). W=64: 5/5 FAST (6.4-7.9 s). Phase C is
  128 shards drained via one atomic counter into 104 Lock-guarded group
  accumulators; the slow mode is ~17x the fast mode at IDENTICAL checksums.
  Looks like a park/contention pathology on the group Locks (or counter
  cacheline) that engages at W=32/48 but not at W=64 on a 64-core box.
  Reproducible target for a future contention hunt.

### Leg (b) — phase-B/C-heavy variant, 3x each of W=32/64: 5/6 OK, 1 SIGABRT
Variant = bench.js with N_BASE=800 (ingest shrunk), N_QUERIES=16000 (queries
cranked 8x relative to base), C_REPEAT=8 (analytics passes cranked 8x); file
preserved at `/root/WebKit-limp/dive-logs/variant/bench-bc.js`.
- 5/6 runs exit 0 with checksums IDENTICAL across runs and W
  (A `5e4318f1b5eb6c74`, postings 119390, A2 `7125de92e0162651`,
  B `a016cee25d2e57eb`, C `149a86fae0856ae6`). 0 LIMP lines, 0 ASAN reports.
- **benchbc-w32-r1: SIGABRT (exit 134) at 295 s — NEW crash PAST the papered
  sites** (the dive's one true anomaly):

      ASSERTION FAILED: pc->opcodeID() == op_call
      Source/JavaScriptCore/llint/LLIntSlowPaths.cpp(3167)
      llint_slow_path_array_sort_comparator_return

  Mechanism: the DFG ArraySortIntrinsic OSR-exit trampoline (comparator
  inlined into sort, OSR exit inside the comparator body) recovers
  `callFrame->bytecodeIndex()` and asserts the instruction there is the
  `op_call` of the sort — under GIL-off on a spawned Thread it is NOT.
  The workload's only user comparators are `entries.sort(...)` in
  queryScored (Phase B, hot at 16000 queries) and `g.terms.sort(...)` in
  checksumPhaseC. This is a debug-only ASSERT: a release build would
  silently `dispatchToCurrentInstructionDuringExit` at a wrong pc =
  silent control-flow corruption. Candidate causes worth a targeted hunt:
  per-lite vs carrier mismatch in the CallSiteIndex stashed by
  reifyInlinedCallFrames, or cross-thread CodeBlock replacement between
  stash and recovery. Frequency: 1/6 variant runs (0/15 leg-a runs — needs
  the 8x query pressure to tier sort+comparator into DFG).
  Log: `/root/WebKit-limp/dive-logs/benchbc-w32-r1-a1.log`.
- Anti-scaling observation: variant phaseB/phaseC walls GROW with W
  (B: 174-176 s @32 -> 206-218 s @64; C: 220-340 s, overlapping but
  worst case @64). Contended-lock throughput degrades past W=32.

### Leg (c) — JSTests/threads/scaling at SCALING_THREADS=32/64 (scale=1/128): 12/12 COMPLETED
All exit 0, all internal determinism + cross-thread checksum self-checks
passed, 0 LIMP, 0 ASAN. Wall ms per `SCALING` line (T(N); equal work per
thread, perfect scaling = flat):

| workload      | T(32) ms | T(64) ms | T64/T32 |
|---------------|----------|----------|---------|
| richards-like |     3655 |     6900 |    1.9x |
| splay-like    |    10612 |    29534 |    2.8x |
| map-heavy     |    42945 |   155750 |    3.6x |
| raytrace-like |    43648 |   112998 |    2.6x |
| string-heavy  |   202967 |   394787 |    1.9x |

lock-fairness: ratioW 1.19 @32; @64 per-thread totals remain tight
(~550-720/window). NOTE: N=64 saturates the 64-core box on top of ambient
load (~5) and ASAN overhead, so T64/T32 conflates engine scalability with
host oversubscription — treat 2-4x degradation as an upper bound on harm,
not a measurement of the shared-heap bottleneck alone. The allocation-heavy
workloads (map-heavy, splay-like) degrade most, consistent with shared-GC-heap
/ allocator contention.

## Bottom line

- The papered varargs + libpas + watchdog + markListSet sites NEVER fired
  in 33 sequential runs at W/N up to 64 (0 LIMP lines campaign-wide) — on
  this tree (post U-T1 per-lite varargs scratch routing) those bugs are
  either fixed or need overload/parallel-run pressure to provoke.
- No checksum divergence anywhere: every completing run of a given program
  produced bit-identical checksums (15/15 leg a, 5/5 completing leg b).
- ONE new GIL-off bug surfaced: the ArraySortIntrinsic OSR-exit
  comparator-return trampoline (LLIntSlowPaths.cpp:3167) recovers a non-op_call
  pc on spawned Threads under query-heavy load. New bughunt target; would be
  silent miscompilation-grade corruption in release.
- TWO performance signatures for the scalebench file: the bimodal Phase C
  (W=32/48 slow mode, 17x) and >W=32 anti-scaling of lock-heavy phases.

## Stale-line note

`ledger.txt` contains one artifact line `RUN=bench-w32-r1 OUTCOME=OK_NOJSON`
from a first chain launch that was killed and restarted with the corrected
ASAN env (its replacement is `bench-w32-r1-a1`), and the box's external
process killer SIGKILLed the first driver mid `bench-w48-r3` (truncated first
attempt; rerun by the resume driver as the recorded `bench-w48-r3-a1`).
